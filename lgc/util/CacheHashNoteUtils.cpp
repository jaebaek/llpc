/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Google LLC. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
#include "lgc/CacheHashNoteUtils.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ELF.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/BinaryStreamWriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace lgc;

// -add-hash-to-elf
static cl::opt<bool> AddHashToELF("add-hash-to-elf",
                                  cl::desc("Add a .note section to ELF for hash used to lookup cache"),
                                  cl::init(false));

// The implementation of ELF rewriting is based on "Linux Programmer's Manual ELF(5)".
// In particular, see "Notes (Nhdr)" of the document for the note section.

namespace {

// It is similar to struct NoteHeader defined in llpc/util/vkgcElfReader.h, but
// it allows us to use Elf_Nhdr_Impl<object::ELF64LE>::Align and it does not
// have the size limitation of note name.
using NoteHeader = object::Elf_Nhdr_Impl<object::ELF64LE>;

// =====================================================================================================================
// A section to shift and its new offset.
struct SectionShiftInfo {
  SmallString<64> section;
  ELF::Elf64_Off newOffset;
};

// =====================================================================================================================
// Add an entry to the note section.
//
// @param noteName                : Name of the note entry.
// @param noteDesc                : Description of the note entry.
// @param [out] noteSectionWriter : The .note section where the note entry will be added to.
void addNoteEntry(StringRef noteName, ArrayRef<uint8_t> noteDesc, BinaryStreamWriter &noteSectionWriter) {
  NoteHeader noteHeader = {};
  noteHeader.n_namesz = noteName.size() + 1;
  noteHeader.n_descsz = noteDesc.size();
  // TODO: Define a note type to specify cache hash and llpc version. It must
  // be defined in llvm/include/llvm/BinaryFormat/ELF.h.
  // Note types with values between 0 and 32 (inclusive) are reserved.
  noteHeader.n_type = 0;
  auto error = noteSectionWriter.writeObject(noteHeader);
  assert(!error);

  // Write the note name terminated by zero and zeros for the alignment.
  error = noteSectionWriter.writeCString(noteName);
  assert(!error);
  error = noteSectionWriter.writeFixedString(
      StringRef("\0\0\0", offsetToAlignment(noteSectionWriter.getLength(), Align::Constant<NoteHeader::Align>())));
  assert(!error);

  // Write the note description and zeros for the alignment.
  error = noteSectionWriter.writeBytes(noteDesc);
  assert(!error);
  error = noteSectionWriter.writeFixedString(
      StringRef("\0\0\0", offsetToAlignment(noteSectionWriter.getLength(), Align::Constant<NoteHeader::Align>())));
  assert(!error);
}

// =====================================================================================================================
// Create a note section with the note "llpc_cache_hash" that has the cache hash
// as the description.
//
// Reference: Linux Programmer's Manual ELF(5) "Notes (Nhdr)".
//
// @param cacheHash               : Hash code to be added to the new .note section.
// @param llpcVersion             : LLPC version that gives us a hint how cacheHash
//                                was generated.
// @param [out] noteSectionStream : The new .note section with the cache hash.
void createNoteSectionWithCacheHash(ArrayRef<uint8_t> cacheHash, ArrayRef<uint8_t> llpcVersion,
                                    AppendingBinaryByteStream &noteSectionStream) {
  BinaryStreamWriter noteSectionWriter(noteSectionStream);

  // Write the note header for the cache hash.
  addNoteEntry(CacheHashNoteName, cacheHash, noteSectionWriter);

  // Write the note header for the llpc version note. The llpc version note can
  // be used to determine the hash algorithm.
  addNoteEntry(LlpcVersionNoteName, llpcVersion, noteSectionWriter);
}

// =====================================================================================================================
// Create a note section header for the cache hash note section.
//
// @param offsetOfSection : The offset of the note section.
// @param sectionSize     : The size of the note section.
// @param sectionHeaders  : The ArrayRef for the section headers.
// @returns               : The newly created note section header.
ELF::Elf64_Shdr createNoteSectionHeader(const ELF::Elf64_Off offsetOfSection, const ELF::Elf64_Xword sectionSize,
                                        const ArrayRef<ELF::Elf64_Shdr> sectionHeaders) {
  ELF::Elf64_Shdr noteSectionHeader = {};
  noteSectionHeader.sh_offset = offsetOfSection;
  noteSectionHeader.sh_type = ELF::SHT_NOTE;
  noteSectionHeader.sh_addralign = NoteHeader::Align;
  noteSectionHeader.sh_size = sectionSize;

  // Find an existing note section to reuse its name ".note".
  auto existingNoteSection = find_if(
      sectionHeaders, [](const ELF::Elf64_Shdr &sectionHeader) { return sectionHeader.sh_type == ELF::SHT_NOTE; });
  if (existingNoteSection != sectionHeaders.end())
    noteSectionHeader.sh_name = existingNoteSection->sh_name;

  // TODO: Find a way to add ".note" to the string table when there is no existing ".note" section.
  assert(noteSectionHeader.sh_name != 0);

  return noteSectionHeader;
}

// =====================================================================================================================
// Returns a vector of sections that need to be shifted with their new offset.
//
// @param elf                     : The original ELF.
// @param sectionHeaders          : The ArrayRef for the section headers.
// @param endOfSectionHeaderTable : The offset of the end of the section header table.
//                                All sections after this offset will be shifted.
// @param initialOffset           : The offset at which to place the sections that
//                                need to be shifted.
// @param [out] sectionAndNewOffset  : The shift information of sections. It includes
//                                the contents of sections to be shifted and the
//                                new offset to write those sections.
void getNewOffsetsForSections(const SmallVectorImpl<char> &elf, const ArrayRef<ELF::Elf64_Shdr> sectionHeaders,
                              const ELF::Elf64_Off endOfSectionHeaderTable, ELF::Elf64_Off initialOffset,
                              SmallVectorImpl<SectionShiftInfo> &sectionAndNewOffset) {
  // If a section is located after the section header table (i.e., its offset
  // is bigger than endOfSectionHeaderTable), it will be shifted. We keep the
  // section's contents and its new offset. This information will be used when
  // we rewrite the ELF.
  for (const auto &sectionHeader : sectionHeaders) {
    if (sectionHeader.sh_offset < endOfSectionHeaderTable)
      continue;
    sectionAndNewOffset.push_back(
        {SmallString<64>(StringRef(elf.data() + sectionHeader.sh_offset, sectionHeader.sh_size)),
         alignTo(initialOffset, Align(sectionHeader.sh_addralign))});
    initialOffset = sectionAndNewOffset.back().newOffset + sectionHeader.sh_size;
  }
}

// =====================================================================================================================
// Rewrite ELF to add the new note section and its header for the cache hash.
//
// @param [in/out] elf      : ELF to be rewritten with the new .note section
//                          for the cache hash.
// @param offsetOfNewSectionHeader   : The offset where the new section header will be
//                          placed.
// @param newNoteSectionHeader      : The section header for the new note section.
// @param noteSectionStream       : The note secion containing the cache hash.
// @param sectionAndNewOffset  : The information to shift sections after the
//                          section header table.
void rewriteELFWithCacheHash(SmallVectorImpl<char> &elf, const ELF::Elf64_Off offsetOfNewSectionHeader,
                             const ELF::Elf64_Shdr &newNoteSectionHeader, AppendingBinaryByteStream &noteSectionStream,
                             SmallVectorImpl<SectionShiftInfo> &sectionAndNewOffset) {
  // Strip sections after the section header table.
  elf.resize(offsetOfNewSectionHeader);

  // Increase the number of section headers in ELF header.
  reinterpret_cast<ELF::Elf64_Ehdr *>(elf.data())->e_shnum++;

  // Write the section header for the new note section.
  raw_svector_ostream elfStream(elf);
  elfStream << StringRef(reinterpret_cast<const char *>(&newNoteSectionHeader), sizeof(ELF::Elf64_Shdr));

  // Write zeros for alignment of the note section.
  const ELF::Elf64_Off numberOfZerosAlign = offsetToAlignment(elfStream.tell(), Align::Constant<NoteHeader::Align>());
  elfStream.write_zeros(numberOfZerosAlign);

  // Write the new note section.
  ArrayRef<uint8_t> noteSection;
  auto error = noteSectionStream.readBytes(0, noteSectionStream.getLength(), noteSection);
  assert(!error);
  elfStream << StringRef(reinterpret_cast<const char *>(noteSection.data()), noteSection.size());

  // Sort SectionShiftInfo by the new offset of each section in the increasing
  // order.
  sort(sectionAndNewOffset,
       [](const SectionShiftInfo &i0, const SectionShiftInfo &i1) { return i0.newOffset < i1.newOffset; });

  // Shift the sections after section header table.
  for (const auto &sectionAndNewOffsetInfo : sectionAndNewOffset) {
    elfStream.write_zeros(sectionAndNewOffsetInfo.newOffset - elfStream.str().size());
    elfStream << sectionAndNewOffsetInfo.section.str();
  }
}

} // anonymous namespace

namespace lgc {

// =====================================================================================================================
// Adds a note section with the note "llpc_cache_hash" in the given ELF to keep
// the hash code for the cache.
//
// @param [in/out] elf : ELF to be updated with the new .note section for the
//                      cache hash.
// @param cacheHash    : Hash code to be added to the new .note section.
// @param llpcVersion  : LLPC version that gives us a hint how cacheHash
//                     was generated.
void addHashSectionToElf(SmallVectorImpl<char> &elf, ArrayRef<uint8_t> cacheHash, ArrayRef<uint8_t> llpcVersion) {
  // If '-add-hash-to-elf' option is not enabled, return without any change.
  if (!AddHashToELF)
    return;

  // Get ELF header that contains information for section header table offset
  // and the number of section headers.
  //
  // Reference: http://www.skyfree.org/linux/references/ELF_Format.pdf
  ELF::Elf64_Ehdr ehdr = {};
  memcpy(&ehdr, elf.data(), sizeof(ELF::Elf64_Ehdr));

  // The offset of new section header must be the end of the current section
  // header table.
  const ELF::Elf64_Off offsetOfNewSectionHeader = ehdr.e_shoff + ehdr.e_shnum * sizeof(ELF::Elf64_Shdr);
  const ELF::Elf64_Off endOfNewSectionHeader = offsetOfNewSectionHeader + sizeof(ELF::Elf64_Shdr);

  // The offset of the new note section that has to be aligned to the
  // NoteHeader::Align.
  const ELF::Elf64_Off offsetOfNewSection =
      endOfNewSectionHeader + offsetToAlignment(endOfNewSectionHeader, Align::Constant<NoteHeader::Align>());

  // Create the new note section for cache hash.
  AppendingBinaryByteStream noteSectionStream(support::little);
  createNoteSectionWithCacheHash(cacheHash, llpcVersion, noteSectionStream);

  // Create the note section header for the new cache hash note section.
  ArrayRef<ELF::Elf64_Shdr> sectionHeaders(reinterpret_cast<ELF::Elf64_Shdr *>(elf.data() + ehdr.e_shoff),
                                           ehdr.e_shnum);
  ELF::Elf64_Shdr newNoteSectionHeader =
      createNoteSectionHeader(offsetOfNewSection, noteSectionStream.getLength(), sectionHeaders);

  // Get the shift information of sections after the section header table.
  SmallVector<SectionShiftInfo> sectionAndNewOffset;
  getNewOffsetsForSections(elf, sectionHeaders, offsetOfNewSectionHeader,
                           offsetOfNewSection + noteSectionStream.getLength(), sectionAndNewOffset);

  // Rewrite ELF.
  rewriteELFWithCacheHash(elf, offsetOfNewSectionHeader, newNoteSectionHeader, noteSectionStream, sectionAndNewOffset);
}

} // namespace lgc

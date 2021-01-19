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

constexpr char ZerosForNoteAlign[NoteHeader::Align] = {'\0'};

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
// @param [out] noteEntryWriter : The .note section where the note entry will be added to.
void addNoteEntry(StringRef noteName, ArrayRef<uint8_t> noteDesc, BinaryStreamWriter &noteEntryWriter) {
  NoteHeader noteHeader = {};
  noteHeader.n_namesz = noteName.size() + 1;
  noteHeader.n_descsz = noteDesc.size();
  // TODO: Define a note type to specify cache hash and llpc version. It must
  // be defined in llvm/include/llvm/BinaryFormat/ELF.h.
  // Note types with values between 0 and 32 (inclusive) are reserved.
  noteHeader.n_type = 0;
  auto error = noteEntryWriter.writeObject(noteHeader);
  (void)error;
  assert(!error);

  // Write the note name terminated by zero and zeros for the alignment.
  error = noteEntryWriter.writeCString(noteName);
  assert(!error);
  error = noteEntryWriter.writeFixedString(StringRef(
      ZerosForNoteAlign, offsetToAlignment(noteEntryWriter.getLength(), Align::Constant<NoteHeader::Align>())));
  assert(!error);

  // Write the note description and zeros for the alignment.
  error = noteEntryWriter.writeBytes(noteDesc);
  assert(!error);
  error = noteEntryWriter.writeFixedString(StringRef(
      ZerosForNoteAlign, offsetToAlignment(noteEntryWriter.getLength(), Align::Constant<NoteHeader::Align>())));
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
// @param [out] noteEntryStream : The new .note section with the cache hash.
void prepareNoteEntries(ArrayRef<NoteEntry> notes, const ELF::Elf64_Off newNoteEntryOffset,
                        AppendingBinaryByteStream &noteEntryStream) {
  BinaryStreamWriter noteEntryWriter(noteEntryStream);
  auto error = noteEntryWriter.writeFixedString(
      StringRef(ZerosForNoteAlign, offsetToAlignment(newNoteEntryOffset, Align::Constant<NoteHeader::Align>())));
  (void)error;
  assert(!error);

  // Write the note entries.
  for (auto &note : notes) {
    addNoteEntry(note.name, note.desc, noteEntryWriter);
  }
}

// =====================================================================================================================
// Returns a vector of sections that need to be shifted with their new offset.
//
// @param elf                       : The original ELF.
// @param sectionHeaders            : The ArrayRef for the section headers.
// @param newNoteEntryOffset        : The offset of the new note entry. All sections
//                                  after this offset will be shifted.
// @param initialOffset             : The offset at which to place the sections that
//                                  need to be shifted.
// @returns     : The shift information of sections. It includes
//                                  the contents of sections to be shifted and the
//                                  new offset to write those sections.
SmallVector<SectionShiftInfo> getAndUpdateOffsetsForSections(const SmallVectorImpl<char> &elf,
                                                             const ELF::Elf64_Off newNoteEntryOffset,
                                                             ELF::Elf64_Off lengthToBeShifted,
                                                             MutableArrayRef<ELF::Elf64_Shdr> sectionHeaders) {
  SmallVector<SectionShiftInfo> sectionAndNewOffset;

  // If a section is located after the new note entry it must be shifted.
  for (auto &sectionHeader : sectionHeaders) {
    if (sectionHeader.sh_offset < newNoteEntryOffset)
      continue;
    const auto newOffset = alignTo(sectionHeader.sh_offset + lengthToBeShifted, Align(sectionHeader.sh_addralign));
    sectionAndNewOffset.push_back(
        {SmallString<64>(StringRef(elf.data() + sectionHeader.sh_offset, sectionHeader.sh_size)), newOffset});
    lengthToBeShifted = newOffset - sectionHeader.sh_offset;

    // Update the offset of section pointed by the section header to its new offset.
    sectionHeader.sh_offset = newOffset;
  }
  return sectionAndNewOffset;
}

// =====================================================================================================================
// Rewrite ELF to add the new note section and its header for the cache hash.
//
// @param [in/out] elf        : ELF to be rewritten with the new .note section
//                            for the cache hash.
// @param newNoteEntryOffset  : The offset of the new note entry.
// @param noteEntryStream     : The note secion containing the cache hash.
// @param sectionAndNewOffset : The information to shift sections after the
//                            section header table.
void rewriteELFWithNewNoteEntries(SmallVectorImpl<char> &elf, const ELF::Elf64_Off newNoteEntryOffset,
                                  AppendingBinaryByteStream &noteEntryStream,
                                  SmallVectorImpl<SectionShiftInfo> &sectionAndNewOffset) {
  // Strip sections after the offset for the new note entry.
  elf.resize(newNoteEntryOffset);

  // Write the new note entries.
  raw_svector_ostream elfStream(elf);
  ArrayRef<uint8_t> noteEntry;
  auto error = noteEntryStream.readBytes(0, noteEntryStream.getLength(), noteEntry);
  (void)error;
  assert(!error);
  elfStream << StringRef(reinterpret_cast<const char *>(noteEntry.data()), noteEntry.size());

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

void writeSectionHeaderTable(SmallVectorImpl<char> &elf, const ELF::Elf64_Off sectionHeaderTableOffset,
                             const SmallVectorImpl<ELF::Elf64_Shdr> &sectionHeaderTable) {
  raw_svector_ostream elfStream(elf);
  unsigned minSizeForSectionHeaders = sectionHeaderTableOffset + sizeof(ELF::Elf64_Shdr) * sectionHeaderTable.size();
  if (minSizeForSectionHeaders > elf.size())
    elfStream.write_zeros(minSizeForSectionHeaders - elf.size());
  elfStream.pwrite(reinterpret_cast<const char *>(sectionHeaderTable.data()),
                   sizeof(ELF::Elf64_Shdr) * sectionHeaderTable.size(), sectionHeaderTableOffset);
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
void addHashSectionToElf(SmallVectorImpl<char> &elf, llvm::ArrayRef<NoteEntry> notes) {
  // If '-add-hash-to-elf' option is not enabled, return without any change.
  if (!AddHashToELF)
    return;

  // Get ELF header that contains information for section header table offset
  // and the number of section headers.
  //
  // Reference: http://www.skyfree.org/linux/references/ELF_Format.pdf
  ELF::Elf64_Ehdr *ehdr = reinterpret_cast<ELF::Elf64_Ehdr *>(elf.data());

  // Get the section headers and the existing note section.
  MutableArrayRef<ELF::Elf64_Shdr> sectionHeaders(reinterpret_cast<ELF::Elf64_Shdr *>(elf.data() + ehdr->e_shoff),
                                                  ehdr->e_shnum);
  auto existingNoteSection = find_if(
      sectionHeaders, [](const ELF::Elf64_Shdr &sectionHeader) { return sectionHeader.sh_type == ELF::SHT_NOTE; });

  // Prepare the new note entries to be added to the existing note section.
  const ELF::Elf64_Off newNoteEntryOffset = existingNoteSection->sh_offset + existingNoteSection->sh_size;
  AppendingBinaryByteStream noteEntryStream(support::little);
  prepareNoteEntries(notes, newNoteEntryOffset, noteEntryStream);

  // Get the section before the section header table.
  auto sectionBeforeSectionHeaderTable =
      std::max_element(sectionHeaders.begin(), sectionHeaders.end(),
                       [ehdr](const ELF::Elf64_Shdr &largest, const ELF::Elf64_Shdr &current) {
                         if (current.sh_offset > ehdr->e_shoff)
                           return false;
                         return current.sh_offset > largest.sh_offset;
                       });

  // Get the shift information of sections after the section header table.
  SmallVector<SectionShiftInfo> sectionAndNewOffset =
      getAndUpdateOffsetsForSections(elf, newNoteEntryOffset, noteEntryStream.getLength(), sectionHeaders);

  // Update the size of the existing note section and the section header table offset.
  existingNoteSection->sh_size += noteEntryStream.getLength();
  SmallVector<ELF::Elf64_Shdr> sectionHeaderTable;
  if (ehdr->e_shoff > newNoteEntryOffset) {
    ehdr->e_shoff = sectionBeforeSectionHeaderTable->sh_offset + sectionBeforeSectionHeaderTable->sh_size;
    sectionHeaderTable.append(sectionHeaders.begin(), sectionHeaders.end());
  }

  // Rewrite ELF.
  rewriteELFWithNewNoteEntries(elf, newNoteEntryOffset, noteEntryStream, sectionAndNewOffset);

  // Write section header table.
  if (sectionHeaderTable.size() > 0) {
    writeSectionHeaderTable(elf, ehdr->e_shoff, sectionHeaderTable);
    MutableArrayRef<ELF::Elf64_Shdr> newSectionHeaders(reinterpret_cast<ELF::Elf64_Shdr *>(elf.data() + ehdr->e_shoff),
                                                       ehdr->e_shnum);
  }
}

} // namespace lgc

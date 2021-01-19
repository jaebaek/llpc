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
/**
 ***********************************************************************************************************************
 * @file  CacheHashNoteUtils.h
 * @brief LLPC header file: declaration of lgc:: interface
 *
 * @details The function addNotesToELF adds given note entries to the given ELF.
 ***********************************************************************************************************************
 */

#pragma once
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace lgc {

// Note entry that will be added to the note section.
struct NoteEntry {
  llvm::StringRef name;
  llvm::ArrayRef<uint8_t> desc;
};

// =====================================================================================================================
// Adds given note entries to the given ELF.
//
// ELF layouts before/after adding new ".note" section and its section header
// for cache hash:
//
// |-------------|        |-------------|
// | ELF header  |        | ELF header  |
// |-------------|        |-------------|
// | Sections    |        | Sections    |
// | ...         |        | ...         |
// |-------------|  ==>   |-------------|
// | Section     |        | Section     |
// | headers     |        | headers     |
// | ...         |        | ...         |
// |-------------|        |-------------|
// | Sections    |        | New section |
// | ...         |---|    | header for  |
// |-------------|   |    | hash note   |    This new section header's
//                   |    | section    +|--| offset must be the new
//    Remaining      |    |-------------|  | note section for cache hash
//    sections after |    | New note    |  |
//    section header |    | section for |<-/
//    table must be  |    | cache hash  |
//    shifted        |    |-------------|
//                   \--->| Sections    |
//                        | ...         |
//                        |-------------|
//
// The new note section for the cache hash will contain two note entries:
//  1. Note name with "llpc_cache_hash" and note description with the cache hash used for the cache lookup.
//  2. Note name with "llpc_version" and note description with the LLPC version - both major and minor.
//     The LLPC version information will help us to understand the hash generation algorithm. We have to
//     use a correct hash algorithm for the cache lookup.
//
// For example, if the hash is "4EDBED25 ADF15238 B8C92579 423DA423" and the LLPC version is 45.4
// (the major version is 45=0x2D and the minor version is 4=0x04), the new note section will be
//
// .note (size = 80 bytes)
//  Unknown(0)                (name = llpc_cache_hash  size = 16)
//        0:4EDBED25 ADF15238 B8C92579 423DA423
//  Unknown(0)                (name = llpc_version  size = 8)
//        0:0000002D 00000004
//
void addNotesToELF(llvm::SmallVectorImpl<char> &elf, llvm::ArrayRef<NoteEntry> notes);

} // namespace lgc

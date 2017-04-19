//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#define __STDC_FORMAT_MACROS
#define _CRT_SECURE_NO_WARNINGS
#include <inttypes.h>
#include "Indexer.h"

using std::string;

const static char ROW_DELIMITER = '\n';

namespace Microsoft { namespace MSR { namespace CNTK {

Indexer::Indexer(FILE* file, bool primary, bool skipSequenceIds, char streamPrefix, size_t chunkSize, size_t bufferSize) :
    m_streamPrefix(streamPrefix),
    m_bufferSize(bufferSize),
    m_file(file),
    m_fileOffsetStart(0),
    m_fileOffsetEnd(0),
    m_buffer(new char[bufferSize + 1]),
    m_bufferStart(nullptr),
    m_bufferEnd(nullptr),
    m_pos(nullptr),
    m_done(false),
    m_hasSequenceIds(!skipSequenceIds),
    m_index(chunkSize, primary)
{
    if (m_file == nullptr)
    {
        RuntimeError("Input file not open for reading");
    }
}

void Indexer::RefillBuffer()
{
    if (!m_done)
    {
        size_t bytesRead = fread(m_buffer.get(), 1, m_bufferSize, m_file);
        if (bytesRead == (size_t)-1)
            RuntimeError("Could not read from the input file.");
        if (bytesRead == 0)
        {
            m_done = true;
        }
        else
        {
            m_fileOffsetStart = m_fileOffsetEnd;
            m_fileOffsetEnd += bytesRead;
            m_bufferStart = m_buffer.get();
            m_pos = m_bufferStart;
            m_bufferEnd = m_bufferStart + bytesRead;
        }
    }
}

void Indexer::BuildFromLines()
{
    assert(m_pos == m_bufferStart);
    m_hasSequenceIds = false;
    size_t lines = 0;
    int64_t offset = GetFileOffset();
    while (!m_done)
    {
        m_pos = (char*)memchr(m_pos, ROW_DELIMITER, m_bufferEnd - m_pos);
        if (m_pos)
        {
            auto sequenceOffset = offset;
            offset = GetFileOffset() + 1;
            m_index.AddSequence(SequenceDescriptor{ lines, 1 }, sequenceOffset, offset);
            ++m_pos;
            ++lines;
        }
        else
        {
            RefillBuffer();
        }
    }

    if (offset < m_fileOffsetEnd)
    {
        // There's a number of characters, not terminated by a newline,
        // add a sequence to the index, parser will have to deal with it.
        m_index.AddSequence(SequenceDescriptor{ lines, 1 }, offset, m_fileOffsetEnd);
    }
}

void Indexer::Build(CorpusDescriptorPtr corpus)
{
    if (!m_index.IsEmpty())
    {
        return;
    }

    // Create a lambda to read symbolic or numeric sequence ids,
    // depending on what the corpus expects.
    std::function<bool(size_t&)> tryGetSequenceId;
    if (corpus->IsNumericSequenceKeys())
        tryGetSequenceId = [this](size_t& id) { return TryGetNumericSequenceId(id); };
    else
        tryGetSequenceId = [this, corpus](size_t& id) { return TryGetSymbolicSequenceId(id, corpus->KeyToId); };

    m_index.Reserve(filesize(m_file));

    RefillBuffer(); // read the first block of data
    if (m_done)
    {
        RuntimeError("Input file is empty");
    }

    if ((m_bufferEnd - m_bufferStart > 3) &&
        (m_bufferStart[0] == '\xEF' && m_bufferStart[1] == '\xBB' && m_bufferStart[2] == '\xBF'))
    {
        // input file contains UTF-8 BOM value, skip it.
        m_pos += 3;
        m_fileOffsetStart += 3;
        m_bufferStart += 3;
    }

    // check the first byte and decide what to do next
    if (!m_hasSequenceIds || m_bufferStart[0] == m_streamPrefix)
    {
        // Skip sequence id parsing, treat lines as individual sequences
        // In this case the sequences do not have ids, they are assigned a line number.
        // If corpus expects to have sequence ids as symbolic names we throw.
        if (!corpus->IsNumericSequenceKeys())
            RuntimeError("Corpus expects non-numeric sequence keys but the CTF input file does not have them.");

        BuildFromLines();
        return;
    }

    size_t id = 0;
    int64_t offset = GetFileOffset();
    // read the very first sequence id
    if (!tryGetSequenceId(id))
    {
        RuntimeError("Expected a sequence id at the offset %" PRIi64 ", none was found.", offset);
    }

    auto sequenceOffset = offset;
    size_t previousId = id;
    uint32_t numberOfSamples = 0;
    while (!m_done)
    {
        SkipLine(); // ignore whatever is left on this line.
        offset = GetFileOffset(); // a new line starts at this offset;
        numberOfSamples++;

        if (!m_done && tryGetSequenceId(id) && id != previousId)
        {
            // found a new sequence, which starts at the [offset] bytes into the file
            // adding the previous one to the index.
            m_index.AddSequence(SequenceDescriptor{ previousId, numberOfSamples }, sequenceOffset, offset);

            sequenceOffset = offset;
            previousId = id;
            numberOfSamples = 0;
        }
    }

    m_index.AddSequence(SequenceDescriptor{ previousId, numberOfSamples }, sequenceOffset, m_fileOffsetEnd);
}

void Indexer::SkipLine()
{
    while (!m_done)
    {
        m_pos = (char*)memchr(m_pos, ROW_DELIMITER, m_bufferEnd - m_pos);
        if (m_pos)
        {
            //found a new-line character
            if (++m_pos == m_bufferEnd)
            {
                RefillBuffer();
            }
            return;
        }
        RefillBuffer();
    }
}

bool Indexer::TryGetNumericSequenceId(size_t& id)
{
    bool found = false;
    id = 0;
    while (!m_done)
    {
        char c = *m_pos;
        if (!isdigit(c))
        {
            // Stop as soon as there's a non-digit character
            return found;
        }

        id = id * 10 + (c - '0');
        found = true;
        ++m_pos;

        if (m_pos == m_bufferEnd)
            RefillBuffer();
    }

    // reached EOF without hitting the pipe character,
    // ignore it for not, parser will have to deal with it.
    return false;
}

bool Indexer::TryGetSymbolicSequenceId(size_t& id, std::function<size_t(const std::string&)> keyToId)
{
    bool found = false;
    id = 0;
    std::string key;
    key.reserve(256);
    while (!m_done)
    {
        char c = *m_pos;
        if (isspace(c))
        {
            if (found)
                id = keyToId(key);
            return found;
        }

        key += c;
        found = true;
        ++m_pos;

        if(m_pos == m_bufferEnd)
            RefillBuffer();
    }

    // reached EOF without hitting the pipe character,
    // ignore it for not, parser will have to deal with it.
    return false;
}

void Index::AddSequence(SequenceDescriptor&& sd, size_t startOffsetInFile, size_t endOffsetInFile)
{
    sd.SetSize(endOffsetInFile - startOffsetInFile);

    assert(!m_chunks.empty());
    ChunkDescriptor* chunk = &m_chunks.back();
    if (chunk->m_byteSize > 0 && (chunk->m_byteSize + sd.m_byteSize) > m_maxChunkSize)
    {
        // If the size is exceeded, finalizing the current chunk
        // and creating a new one.
        chunk->m_sequences.shrink_to_fit();

        m_chunks.push_back({});
        chunk = &m_chunks.back();
        chunk->m_id = (ChunkIdType)(m_chunks.size() - 1);
        chunk->m_offset = startOffsetInFile;

        if (CHUNKID_MAX < m_chunks.size())
        {
            RuntimeError("Maximum number of chunks exceeded");
        }
    }

    if (m_trackFirstSamples) // Adding number of samples where the new sequence starts.
        chunk->m_sequenceOffsetInChunkInSamples.push_back(static_cast<uint32_t>(chunk->m_numberOfSamples));

    chunk->m_byteSize += sd.m_byteSize;
    chunk->m_numberOfSequences++;
    chunk->m_numberOfSamples += sd.m_numberOfSamples;
    if (!m_primary)
    {
        auto location = std::make_pair(chunk->m_id, static_cast<uint32_t>(chunk->m_sequences.size()));
        if (location.second != chunk->m_sequences.size())
            RuntimeError("Number of sequences overflow the chunk capacity.");

        m_keyToSequenceInChunk.insert(std::make_pair(sd.m_key, location));
    }

    sd.SetOffsetInChunk(startOffsetInFile - chunk->m_offset);
    chunk->m_sequences.push_back(sd);
}

}}}

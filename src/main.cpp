#include <raylib.h>
#include <cstdint>
#include <iostream>
#include <functional>
#include <fstream>
#include <map>
#include <map>
#include <memory>
#include <optional>
#include <vector>

constexpr int WINDOW_WIDTH = 800;
constexpr int WINDOW_HEIGHT = 800;
const std::string HEADER_TAG = "MThd";

std::string read_tag(const std::vector<uint8_t>& buf, size_t& pos)
{
    std::string tag = "";
    tag += (char)buf[pos];
    tag += (char)buf[pos+1];
    tag += (char)buf[pos+2];
    tag += (char)buf[pos+3];
    pos += 4;
    return tag;
}

// 1-byte read
uint8_t read_u8(const std::vector<uint8_t>& buf, size_t& pos) 
{
    uint8_t val = buf[pos];
    pos += 1;
    return val;
}

// 2-byte big-endian read
uint16_t read_u16_be(const std::vector<uint8_t>& buf, size_t& pos) 
{
    uint16_t val = (uint16_t(buf[pos]) << 8) | buf[pos+1];
    pos += 2;
    return val;
}

// 4-byte big-endian read
uint32_t read_u32_be(const std::vector<uint8_t>& buf, size_t& pos) 
{
    uint32_t val = (uint32_t(buf[pos])   << 24) |
                   (uint32_t(buf[pos+1]) << 16) |
                   (uint32_t(buf[pos+2]) << 8)  |
                    uint32_t(buf[pos+3]);
    pos += 4;
    return val;
}

int readMidiHeader(const std::vector<uint8_t>& buf, size_t& pos, uint16_t& fmt, uint16_t& trk, uint16_t& div)
{
    std::string tag = read_tag(buf, pos);
    if (tag != "MThd") return 1;
    uint16_t len = read_u32_be(buf, pos);
    //printf("%u\n", len);
    if (len != 6) return 1;
    fmt = read_u16_be(buf, pos);
    trk = read_u16_be(buf, pos);
    div = read_u16_be(buf, pos);
    return 0;
}

struct MidiEvent {
    uint32_t dt;
    uint8_t status, data1, data2;
};

uint32_t readVlqDt(const std::vector<uint8_t>& buf, size_t& pos)
{
    uint32_t value = 0;
    uint8_t byte;
    do
    {
        byte = buf[pos++];
        value = (value << 7) | (byte & 0x7F);
    } while (byte & 0x80);
    return value;
}

std::optional<MidiEvent> readMetaEvent(const std::vector<uint8_t>& buf, size_t& pos)
{
    pos += 1; // skip 0xFF
    uint8_t type = read_u8(buf, pos);
    uint32_t len = readVlqDt(buf, pos);
    pos += len; // skip data bytes
    return std::nullopt;
}

std::optional<MidiEvent> readSysexEvent(const std::vector<uint8_t>& buf, size_t& pos)
{
    pos += 1; // skip 0xF0
    uint32_t len = readVlqDt(buf, pos);
    pos += len; // skip data bytes
    return std::nullopt;
}

std::optional<MidiEvent> readChannelEvent(const std::vector<uint8_t>& buf, size_t& pos, uint8_t running_status, uint32_t dt)
{
    uint8_t command = running_status & 0xF0;
    uint8_t channel = running_status & 0x0F;
    uint8_t data1 = read_u8(buf, pos);
    uint8_t data2 = 0;

    // only commands Program Change(0xC0) and Channel Pressure(0xD0) have 1 data byte, rest have 2
    if (command != 0xC0 && command != 0xD0) data2 = read_u8(buf, pos);
    
    return MidiEvent{dt, running_status, data1, data2};
}

std::optional<MidiEvent> readMidiEvent(const std::vector<uint8_t>& buf, size_t& pos, uint8_t& running_status)
{
    uint32_t dt = readVlqDt(buf, pos);
    uint8_t byte = buf[pos];

    if (byte >= 0x80) // high bit set -> new status byte, consume it
    {
        if (byte == 0xFF) // meta event
        {
            return readMetaEvent(buf, pos);
        }
        else if (byte == 0xF0) // sysex event
        {
            return readSysexEvent(buf, pos);
        }
        else // channel event status byte
        {
            // update running status byte
            running_status = byte;
            pos += 1;
        }
    }
    // else high bit clear -> handle channel event using running status
    return readChannelEvent(buf, pos, running_status, dt);
}

int readMidiTrack(const std::vector<uint8_t>& buf, size_t& pos, std::vector<MidiEvent>& events)
{
    std::string tag = read_tag(buf, pos);
    if (tag != "MTrk") return 1;
    uint32_t len = read_u32_be(buf, pos);
    size_t track_end = pos + static_cast<size_t>(len);
    uint8_t running_status = 0x00;

    while (pos < track_end) 
    {
        auto event = readMidiEvent(buf, pos, running_status);
        if (event.has_value()) events.push_back(event.value());
    }

    return 0;
}

struct Note {
    uint8_t channel;
    uint8_t pitch;
    uint8_t velocity;
    float   startTime;
    float   endTime;
};

float timeInSeconds(uint32_t ticks, uint32_t tempo, uint16_t division)
{
    return ticks * (static_cast<float>(tempo) / division) / 1000000.0f;
}

std::vector<Note> buildNoteList(std::vector<MidiEvent> events, uint32_t tempo, uint16_t division)
{
    std::vector<Note> notes;
    std::map<std::pair<uint8_t, uint8_t>, std::pair<MidiEvent, uint32_t>> unmatchedEvents;

    uint32_t accumulatedTicks = 0;
    for (const MidiEvent& event : events)
    {
        accumulatedTicks += event.dt;
        uint8_t command = event.status & 0xF0;
        uint8_t channel = event.status & 0x0F;
        uint8_t pitch = event.data1;
        uint8_t velocity = event.data2;
        if (command == 0x90)
        {
            // new note on event
            unmatchedEvents.insert({{channel, pitch}, {event, accumulatedTicks}});
        }
        else if (command == 0x80 && unmatchedEvents.contains({channel, pitch}))
        {
            // found matched note off event
            auto key = std::make_pair(channel, pitch);
            MidiEvent onNoteEvent = unmatchedEvents.at(key).first;
            uint32_t startTicks = unmatchedEvents.at(key).second;
            unmatchedEvents.erase(key);

            uint8_t onNoteVelocity = onNoteEvent.data2;
            float startTime = timeInSeconds(startTicks, tempo, division);
            float endTime = timeInSeconds(accumulatedTicks, tempo, division);
            notes.emplace_back(channel, pitch, onNoteVelocity, startTime, endTime);
        }
        else
        {
            printf("unmatched event\n");
        }
    }
    return notes;
}

void drawNote(Vector2 center, Note note, float gap)
{
    uint8_t numPitches = 12;
    float angleOffset = -90.0f;
    float sectorAngle = 360.0f / numPitches;
    float ringWidth = 10.0f;
    float baseRadius = 50.0f;

    uint8_t octave = note.pitch / numPitches;
    uint8_t pitchClass = note.pitch % numPitches;
    float innerRadius = baseRadius + (octave * ringWidth) + (gap / 2);
    float outerRadius = baseRadius + ((octave + 1) * ringWidth) - (gap / 2);
    float startAngle = (pitchClass * sectorAngle) + angleOffset + (gap / 2);
    float endAngle = ((pitchClass + 1) * sectorAngle) + angleOffset - (gap / 2);

    DrawRing(center, innerRadius, outerRadius, startAngle, endAngle, 8, RED);
}

int main()
{   
    // FILE READING
    std::ifstream file("assets/midi/happy_birthday.mid", std::ios::binary);
    if (!file) {
        std::cerr << "Error: could not open MIDI file\n";
        return 1;
    }

    // Read the whole file into a buffer upfront — easier to work with
    std::vector<uint8_t> buffer{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    };

    //for (int i = 0; i < 14; i++) printf("%02X ", buffer[i]);

    size_t bufferPointer = 0;

    // READ HEADER
    uint16_t format;
    uint16_t track_count;
    uint16_t division;

    size_t pos_start = bufferPointer; // for debug

    int rc = readMidiHeader(buffer, bufferPointer, format, track_count, division);
    if (rc) return 1;
    
    printf("format=%u tracks=%u division=%u\n", format, track_count, division);

    // READ TRACK
    std::vector<MidiEvent> events;
    readMidiTrack(buffer, bufferPointer, events);
    // for (MidiEvent event : events)
    // {
    //     std::cout << "dt="       << event.dt
    //               << " status="  << std::hex << (int)event.status
    //               << " data1="  << std::hex << (int)event.data1
    //               << " data2="    << std::dec << (int)event.data2
    //               << "\n";
    // }

    uint32_t tempo = 500000;
    std::vector<Note> notes = buildNoteList(events, tempo, division);
    for (Note note : notes)
    {
    std::cout << "startTime=" << note.startTime
              << " endTime=" << note.endTime
              << " channel=" << static_cast<int>(note.channel)
              << " pitch=" << static_cast<int>(note.pitch)
              << "\n";
    }

    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "synesthesia");

    SetTargetFPS(60);

    Vector2 center = {WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2};
    
    std::multimap<float, Note> activeNotes;
    uint32_t noteIndex = 0;
    float timer = 0.0f;

    while (!WindowShouldClose())
    {        
        timer += GetFrameTime();

        BeginDrawing();

        ClearBackground(BLACK);

        // update active notes buffer
        // 1 - add starting notes
        while (noteIndex < notes.size() &&
                notes[noteIndex].startTime <= timer)
        {
            const Note& nextNote = notes[noteIndex];
            activeNotes.insert({nextNote.endTime, nextNote});
            noteIndex++;
        }

        // 2 - delete ended notes
        while (!activeNotes.empty() && 
                activeNotes.begin()->first <= timer)
            activeNotes.erase(activeNotes.begin());

        // draw active notes
        for (const auto& entry : activeNotes)
            drawNote(center, entry.second, 2.0f);

        EndDrawing();
    }
    
    CloseWindow();
}

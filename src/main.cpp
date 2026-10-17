#include <raylib.h>
#include <algorithm>
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
    uint32_t absoluteTick;
    uint8_t status;
    uint8_t data1;
    uint8_t data2;
    uint8_t voice; // replaces channel (format 0) or track (format 1) as the voice identifier
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

bool readMetaEvent(const std::vector<uint8_t>& buf, size_t& pos, uint32_t& absoluteTick,
                   std::vector<std::pair<uint32_t, uint32_t>>& tempoHistory)
{
    printf("readMetaEvent entry pos=%zu\n", pos);
    pos += 1; // skip 0xFF
    uint8_t type = read_u8(buf, pos);
    uint32_t len = readVlqDt(buf, pos);

    if (type == 0x51 && len == 3)
    {

        uint32_t tempo = (uint32_t(buf[pos])   << 16) |
                         (uint32_t(buf[pos+1]) << 8)  |
                          uint32_t(buf[pos+2]);
        tempoHistory.emplace_back(absoluteTick, tempo);
        // printf("reading tempo: %u at %u \n", tempo, absoluteTick);
    }

    printf("type=%02X len=%u pos=%zu\n", type, len, pos);

    pos += len; // skip data bytes
    printf("readMetaEvent exit pos=%zu\n", pos);
    return 1;
}

bool readSysexEvent(const std::vector<uint8_t>& buf, size_t& pos)
{
    pos += 1; // skip 0xF0
    uint32_t len = readVlqDt(buf, pos);
    pos += len; // skip data bytes
    printf("sysex len=%u pos=%zu\n", len, pos);  // add temporarily
    return 1;
}

bool readChannelEvent(const std::vector<uint8_t>& buf, size_t& pos, uint8_t running_status, 
                      uint8_t& status, uint8_t& data1, uint8_t& data2)
{
    data1 = read_u8(buf, pos);
    data2 = 0;
    uint8_t command = running_status & 0xF0;

    // only commands Program Change(0xC0) and Channel Pressure(0xD0) have 1 data byte, rest have 2
    if (command != 0xC0 && command != 0xD0) data2 = read_u8(buf, pos);
    
    return 0;
}

bool readMidiEvent(const std::vector<uint8_t>& buf, size_t& pos, uint8_t& running_status, uint32_t absoluteTick, std::vector<std::pair<uint32_t, uint32_t>>& tempoHistory,
                   uint32_t& dt, uint8_t& status, uint8_t& data1, uint8_t& data2)
{
    dt = readVlqDt(buf, pos);
    uint8_t byte = buf[pos];

    if (byte >= 0x80) // high bit set -> new status byte, consume it
    {
        if (byte == 0xFF) // meta event
        {
            uint32_t eventAbsoluteTick = absoluteTick + dt;
            readMetaEvent(buf, pos, eventAbsoluteTick, tempoHistory); //////////
            return false;
        }
        else if (byte == 0xF0) // sysex event
        {
            readSysexEvent(buf, pos);
            return false;
        }
        else // channel event status byte
        {
            // update running status byte
            running_status = byte;
            pos += 1;
        }
    }
    // else high bit clear -> handle channel event using running status
    status = running_status;
    readChannelEvent(buf, pos, running_status, status, data1, data2);
    return true;
}

int readMidiTrack(const std::vector<uint8_t>& buf, size_t& pos, std::vector<MidiEvent>& events, uint16_t trackIndex, uint16_t format,
                  std::vector<std::pair<uint32_t, uint32_t>>& tempoHistory)
{
    std::string tag = read_tag(buf, pos);
    if (tag != "MTrk") return 1;
    uint32_t len = read_u32_be(buf, pos);
    printf("track len=%u track_end=%zu buf_size=%zu\n", 
           len, pos + len, buf.size());
    size_t track_end = pos + static_cast<size_t>(len);
    uint8_t running_status = 0x00;

    uint32_t absoluteTick = 0;
    while (pos < track_end) 
    {
        // size_t pos_before = pos;

        uint32_t dt; uint8_t status, data1, data2, voice;
        bool isChannelEvent = readMidiEvent(buf, pos, running_status, absoluteTick, tempoHistory, dt, status, data1, data2);

        // printf("pos before=%zu pos after=%zu bytes consumed=%zu\n",
        //    pos_before, pos, pos - pos_before);

        absoluteTick += dt;
        if (isChannelEvent) 
        {
            voice = (format == 1) ? trackIndex : (status & 0x0F); // voice resolution
            events.emplace_back(absoluteTick, status, data1, data2, voice);
        }
    }
    printf("loop exited: pos=%zu track_end=%zu\n", pos, track_end);
    return 0;
}

struct Note {
    uint8_t voice;
    uint8_t channel;
    uint8_t pitch;
    uint8_t velocity;
    double   startTime;
    double   endTime;
};

double timeInSeconds(uint32_t ticks, uint32_t tempo, uint16_t division)
{
    return ticks * (static_cast<double>(tempo) / division) / 1000000.0f;
}

std::vector<Note> buildNoteList(std::vector<MidiEvent> events, std::vector<std::pair<uint32_t, uint32_t>>& tempoHistory, uint16_t division)
{
    std::vector<Note> notes;
    std::map<std::pair<uint8_t, uint8_t>, std::pair<MidiEvent, double>> unmatchedEvents;
    uint32_t tempoIndex = 0;
    uint32_t tickOfLastTempoChange = 0;
    uint32_t currentTempo = 500000;
    double timeOfLastTempoChange = 0.0f;

    if (!tempoHistory.empty())
    {
        currentTempo          = tempoHistory[0].second;
        tickOfLastTempoChange = tempoHistory[0].first;   // usually 0
    }

    for (const MidiEvent& event : events)
    {
        // poll event and extract fields
        uint8_t voice = event.voice;
        uint8_t command = event.status & 0xF0;
        uint8_t channel = event.status & 0x0F;
        uint8_t pitch = event.data1;
        uint8_t velocity = event.data2;

        // check for and update tempo change
        uint32_t eventAbsoluteTick = event.absoluteTick;
        while (!tempoHistory.empty() && tempoIndex < tempoHistory.size() - 1) // within bounds check
        {
            auto [tickOfNextTempoChange, nextTempo] = tempoHistory[tempoIndex + 1];
            if (tickOfNextTempoChange <= eventAbsoluteTick) // event polled is after tempo change
            {
                printf("tempo changed from %u to %u\n", currentTempo, nextTempo);
                timeOfLastTempoChange = timeOfLastTempoChange + timeInSeconds(tickOfNextTempoChange - tickOfLastTempoChange, currentTempo, division);
                tickOfLastTempoChange = tickOfNextTempoChange;
                currentTempo = nextTempo;
                tempoIndex += 1;
            }
            else break;
        }

        // match notes
        if (command == 0x90 && velocity > 0)
        {
            // new note-on event: add to pending
            double timeSinceTempoChange = timeInSeconds(event.absoluteTick - tickOfLastTempoChange, currentTempo, division);
            double startTime = timeOfLastTempoChange + timeSinceTempoChange;
            unmatchedEvents.insert({{voice, pitch}, {event, startTime}});
        }
        else if ((command == 0x80 || (command == 0x90 && velocity == 0))
                && unmatchedEvents.contains({voice, pitch}))
        {
            // found matched note-off event: create MidiEvent, add to events vector
            auto key = std::make_pair(voice, pitch);
            auto noteOntuple = unmatchedEvents.at(key);
            MidiEvent noteOnEvent = std::get<0>(noteOntuple);
            double startTime = std::get<1>(noteOntuple);
            unmatchedEvents.erase(key);
            // printf("found end note\n");

            uint8_t noteOnVelocity = noteOnEvent.data2;
            double timeSinceTempoChange = timeInSeconds(event.absoluteTick - tickOfLastTempoChange, currentTempo, division);
            double endTime = timeOfLastTempoChange + timeSinceTempoChange;
            notes.emplace_back(voice, channel, pitch, noteOnVelocity, startTime, endTime);
            printf("matched note with tempo %u : startTick: %u, endTick: %u, start: %f, end: %f, voice: %d, pitch: %d \n", currentTempo, noteOnEvent.absoluteTick, event.absoluteTick, startTime, endTime, voice, pitch);
        }
        else
        {
            printf("unmatched event: \n");
            printf("dt: %d, command: %02X, voice: %02X, pitch: %02X \n", event.absoluteTick, command, voice, pitch);
        }

    }

    // debug
    if (!unmatchedEvents.empty())
    {
        printf("unmatched note ons: %zu\n", unmatchedEvents.size());
        for (auto& [key, pair] : unmatchedEvents)
            printf("  voice=%d pitch=%d tick=%u\n", key.first, key.second, pair.first.absoluteTick);
    }

    // sort notes chronologically
    std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b)
    {
        return a.startTime < b.startTime;
    });

    return notes;
}

const Color VOICE_COLORS[16] = {
    {235, 235, 235, 255},   // 12 white
    {235, 64,  52,  255},   // 0  red
    {52,  235, 94,  255},   // 4  green
    {52,  92,  235, 255},   // 7  blue
    {235, 211, 52,  255},   // 2  yellow
    {235, 140, 52,  255},   // 1  orange
    {52,  235, 195, 255},   // 5  teal
    {52,  174, 235, 255},   // 6  sky blue
    {130, 235, 52,  255},   // 3  lime
    {112, 52,  235, 255},   // 8  indigo
    {189, 52,  235, 255},   // 9  violet
    {235, 52,  174, 255},   // 10 pink
    {235, 52,  92,  255},   // 11 rose
    {180, 180, 180, 255},   // 13 light grey
    {100, 100, 100, 255},   // 14 dark grey
    {255, 215, 0,   255},   // 15 gold
};

float computeOpacity(const Note& note, double currentTime)
{
    float duration  = static_cast<float>(note.endTime - note.startTime);
    float age       = static_cast<float>(currentTime - note.startTime);
    float position  = age / duration;   // 0.0 → 1.0

    const float FADE_IN  = 0.5f;        // first 10% of note
    const float FADE_OUT = 0.5f;        // last 20% of note

    if (position < FADE_IN)
        return position / FADE_IN;                      // fade in
    else if (position > (1.0f - FADE_OUT))
        return (1.0f - position) / FADE_OUT;            // fade out
    else
        return 1.0f;                                    // full opacity
}

void drawNote(Vector2 center, Note note, double currentTime, float gap)
{
    uint8_t numPitches = 12;
    float angleOffset = -90.0f;
    float sectorAngle = 360.0f / numPitches;
    float ringWidth = 40.0f;
    float baseRadius = 0.0f;
    float radiusGap = 20.0f;

    uint8_t octave = note.pitch / numPitches;
    uint8_t pitchClass = note.pitch % numPitches;
    float innerRadius = baseRadius + (octave * ringWidth) + (radiusGap / 2);
    float outerRadius = baseRadius + ((octave + 1) * ringWidth) - (radiusGap / 2);
    float startAngle = (pitchClass * sectorAngle) + angleOffset + (gap / 2);
    float endAngle = ((pitchClass + 1) * sectorAngle) + angleOffset - (gap / 2);

    float opacity = computeOpacity(note, currentTime);
    Color base  = VOICE_COLORS[note.voice % 16];
    Color noteColor = {
        base.r, base.g, base.b,
        static_cast<uint8_t>(opacity * 255)
    };

    DrawRing(center, innerRadius, outerRadius, startAngle, endAngle, 8, noteColor);
}

int main(int argc, char* argv[])
{   
    // FILE READING

    if (argc < 2)
    {
        fprintf(stderr, "Usage: synesthesia <midi_file>\n");
        return 1;
    }

    std::string midiPath = argv[1];
    printf("loading: %s\n", midiPath.c_str());

    // open file using the path argument
    std::ifstream file(midiPath, std::ios::binary);
    if (!file)
    {
        fprintf(stderr, "Error: could not open %s\n", midiPath.c_str());
        return 1;
    }

    // Read the whole file into a buffer upfront — easier to work with
    std::vector<uint8_t> buffer{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    };

    // for (int i = 0; i < buffer.size(); i++) printf("%02X ", buffer[i]);

    size_t bufferPointer = 0;

    // READ HEADER
    uint16_t format;
    uint16_t track_count;
    uint16_t division;

    int rc = readMidiHeader(buffer, bufferPointer, format, track_count, division);
    if (rc) return 1;
    
    printf("format=%u tracks=%u division=%u\n", format, track_count, division);

    std::vector<MidiEvent> events;

    std::vector<std::pair<uint32_t, uint32_t>> tempoHistory;
    // uint32_t tempo = 500000;

    // READ TRACKS
    for (uint16_t trackIndex = 0; trackIndex < track_count; trackIndex++)
    {
        printf("Track %d\n", trackIndex);
        // printf("expecting MTrk at pos %zu: %02X %02X %02X %02X\n",
        //     bufferPointer,
        //     buffer[bufferPointer],   buffer[bufferPointer+1],
        //     buffer[bufferPointer+2], buffer[bufferPointer+3]);
        readMidiTrack(buffer, bufferPointer, events, trackIndex, format, tempoHistory);
        printf("bufferPointer=%zu\n", bufferPointer);
        for (MidiEvent event : events)
            printf("dt: %u, status: %02X, data1: %02X, data2: %u, voice: %u\n", event.absoluteTick, event.status, event.data1, event.data2, event.voice);
    }

    // printf("tempo read: %u", tempo);

    // sort events vector
    std::sort(events.begin(), events.end(), [](const MidiEvent& a, const MidiEvent& b)
    {
        if (a.absoluteTick != b.absoluteTick) return a.absoluteTick < b.absoluteTick;

        // same tick: Note Offs (0x80) before Note Ons (0x90)
        // ensures reset Note Offs are processed when pending is empty
        // so they become unmatched rather than consuming genuine Note Ons
        uint8_t commandA = a.status & 0xF0;
        uint8_t commandB = b.status & 0xF0;
        return commandA < commandB;  // 0x80 < 0x90
    });

    std::sort(tempoHistory.begin(), tempoHistory.end(),
    [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    printf("tempoHistory:\n");
    for (std::pair<uint32_t, uint32_t> pair: tempoHistory)
        printf("absoluteTick: %u, tempo: %u \n", pair.first, pair.second);

    std::vector<Note> notes = buildNoteList(events, tempoHistory, division);

    for (Note note : notes)
    {
        printf("start: %f, end: %f, voice: %d, pitch: %d \n", note.startTime, note.endTime, note.voice, note.pitch);
    }

    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "synesthesia");

    SetTargetFPS(60);

    Vector2 center = {WINDOW_WIDTH / 2, WINDOW_HEIGHT / 2};
    
    std::multimap<double, Note> activeNotes;
    uint32_t noteIndex = 0;
    // float timer = 0.0f;
    double playbackStartTime = GetTime();


    while (!WindowShouldClose())
    {        
        // float dt = GetFrameTime();
        // timer += dt;
        double timer = GetTime() - playbackStartTime;

        BeginDrawing();

        ClearBackground(BLACK);

        // update active notes buffer

        // 1 - delete ended notes
        while (!activeNotes.empty() && 
                activeNotes.begin()->first <= timer)
            activeNotes.erase(activeNotes.begin());

        // 2 - add starting notes
        while (noteIndex < notes.size() &&
                notes[noteIndex].startTime <= timer)
        {
            const Note& nextNote = notes[noteIndex];
            activeNotes.insert({nextNote.endTime, nextNote});
            noteIndex++;
        }

        // draw active notes
        // printf("drawing notes at %f\n", timer);
        for (const auto& entry : activeNotes)
        {
            drawNote(center, entry.second, timer, 4.0f);
            // std::cout << "startTime=" << entry.second.startTime
            //     << " endTime=" << entry.second.endTime
            //     << " voice=" << static_cast<int>(entry.second.voice)
            //     << " pitch=" << static_cast<int>(entry.second.pitch)
            //     << "\n";
        }
        EndDrawing();
    }
    
    CloseWindow();
}

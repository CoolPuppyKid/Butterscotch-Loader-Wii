#pragma once

#include "../audio_system.h"
#include "wii_utils.h"
#include "common.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <asndlib.h>

#define WII_MAX_SOUND_INSTANCES 16
#define WII_SOUND_INSTANCE_ID_BASE 100000
#define WII_AUDIO_STREAM_INDEX_BASE 300000
#define WII_OUTPUT_FREQ 22050
#define WII_MAX_MUSIC_STREAMS 4
#define WII_MAX_EXTERNAL_STREAMS 8
#define WII_STREAM_CHUNK_FRAMES 4096
#define WII_STREAM_MAX_CHANNELS 2
#define WII_STREAM_CHUNK_SAMPLES (WII_STREAM_CHUNK_FRAMES * WII_STREAM_MAX_CHANNELS)
#define WII_STREAM_BUFFER_BYTES (WII_STREAM_CHUNK_SAMPLES * sizeof(int16_t))
#define WII_STREAM_ADPCM_CHUNK_BYTES (WII_STREAM_CHUNK_FRAMES / 2)

typedef struct {
    uint16_t audoIndex;
    uint32_t flags;
    int16_t volume;
    int16_t pitch;
} WiiSondEntry;

typedef struct {
    uint32_t dataOffset;
    uint32_t dataSize;
    uint16_t sampleRate;
    uint8_t channels;
    uint8_t bitsPerSample;
    uint8_t format;
    uint32_t sampleCount;
} WiiAudoEntry;

typedef struct {
    char* name;
    uint32_t dataOffset;
    uint32_t dataSize;
    uint16_t sampleRate;
    uint8_t channels;
    uint8_t format;
    uint32_t sampleCount;
} WiiMusEntry;

typedef struct {
    bool active;
    char* name;
    char* filePath;
} WiiExternalStreamEntry;

// ===[ Sound Instance ]===

typedef struct {
    bool active;
    int32_t soundIndex;
    int32_t audoIndex;
    int32_t instanceId;
    int32_t priority;
    bool loop;
    bool paused;
    int voice;            // ASND voice slot (0-15)
    float pitch;
    float sondPitch;
    float currentGain;
    float targetGain;
    float startGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float sondVolume;
    uint32_t sampleRate;
    uint8_t channels;
    int16_t* pcmData;     // decoded PCM (owned by this instance)
    uint32_t pcmBytes;
} WiiSoundInstance;

// ===[ Streaming Music Instance ]===

typedef struct {
    bool active;
    int32_t soundIndex;
    int32_t audoIndex;
    int32_t instanceId;
    int32_t priority;
    bool loop;
    bool paused;
    int voice;
    bool isOggStream;

    float currentGain;
    float targetGain;
    float startGain;
    float fadeTimeRemaining;
    float fadeTotalTime;
    float sondVolume;
    float pitch;
    float sondPitch;

    // ADPCM streaming state
    uint32_t fileStartOffset;
    uint32_t fileEndOffset;
    uint32_t fileOffset;
    int32_t decoderPredictor;
    int32_t decoderStepIndex;
    bool endOfTrack;
    void* vorbis;
    uint32_t sampleRate;
    uint8_t channels;
    float lengthSeconds;

    // Double-buffered decoded PCM for ASND
    int16_t* buffers[2];
    uint32_t bufferFrameCount[2];
    int activeBuffer;
    bool needsRefill;
} WiiMusicStream;

// ===[ Wii Audio System ]===

typedef struct {
    AudioSystem base;

    uint16_t sondEntryCount;
    uint16_t audoEntryCount;
    uint16_t musEntryCount;
    WiiSondEntry* sondEntries;
    WiiAudoEntry* audoEntries;
    WiiMusEntry* musEntries;
    WiiExternalStreamEntry externalStreams[WII_MAX_EXTERNAL_STREAMS];

    FILE* soundsFile;
    DataWin* dataWin;
    FileSystem* fileSystem;
    bool bankAvailable;

    WiiSoundInstance instances[WII_MAX_SOUND_INSTANCES];
    int32_t nextInstanceCounter;

    WiiMusicStream musicStreams[WII_MAX_MUSIC_STREAMS];

    float masterGain;
    bool initialized;
} WiiAudioSystem;

WiiAudioSystem* WiiAudioSystem_create(void);

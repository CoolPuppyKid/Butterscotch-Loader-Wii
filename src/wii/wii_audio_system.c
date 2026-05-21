#include "wii_audio_system.h"
#include "../utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ogc/cache.h>
#include <asndlib.h>
#include "stb_ds.h"

#ifdef IS_BIG_ENDIAN
#define STB_VORBIS_BIG_ENDIAN
#endif
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_vorbis.c"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
#include "wii_log.h"

#define fprintf(stream, ...) WiiLog_fprintf((stream), __VA_ARGS__)

#define WII_ENABLE_EMBEDDED_AUDO_FALLBACK 0

static const int16_t IMA_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14,
    16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

static const int8_t IMA_INDEX_TABLE[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static WiiAudioSystem* g_wiiAudio = NULL;

static void streamVoiceCallback(s32 voice) {
    if (!g_wiiAudio) return;
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        WiiMusicStream* stream = &g_wiiAudio->musicStreams[i];
        if (stream->active && stream->voice == voice) {
            stream->needsRefill = true;
            return;
        }
    }
}

static uint32_t imaAdpcmDecodeBlock(const uint8_t* adpcmData, uint32_t adpcmSize, int16_t* outPcm, int32_t* predictor, int32_t* stepIndex) {
    uint32_t samplesWritten = 0;
    for (uint32_t i = 0; i < adpcmSize; i++) {
        uint8_t byte = adpcmData[i];
        for (int nibbleIdx = 0; nibbleIdx < 2; nibbleIdx++) {
            uint8_t nibble = (nibbleIdx == 0) ? (byte & 0x0F) : ((byte >> 4) & 0x0F);
            int32_t step = IMA_STEP_TABLE[*stepIndex];
            int32_t delta = step >> 3;
            if (nibble & 1) delta += step >> 2;
            if (nibble & 2) delta += step >> 1;
            if (nibble & 4) delta += step;
            if (nibble & 8) delta = -delta;
            *predictor += delta;
            if (*predictor > 32767) *predictor = 32767;
            if (*predictor < -32768) *predictor = -32768;
            *outPcm++ = (int16_t) *predictor;
            samplesWritten++;
            *stepIndex += IMA_INDEX_TABLE[nibble];
            if (*stepIndex < 0) *stepIndex = 0;
            if (*stepIndex > 88) *stepIndex = 88;
        }
    }
    return samplesWritten;
}

static void imaAdpcmDecode(const uint8_t* adpcmData, uint32_t adpcmSize, int16_t* outPcm) {
    int32_t predictor = 0;
    int32_t stepIndex = 0;
    imaAdpcmDecodeBlock(adpcmData, adpcmSize, outPcm, &predictor, &stepIndex);
}

// ===[ SOUNDBNK.BIN Parser ]===

static bool readExact(FILE* f, void* out, size_t size, const char* what) {
    if (fread(out, 1, size, f) == size) return true;
    fprintf(stderr, "WiiAudioSystem: truncated SOUNDBNK.BIN while reading %s\n", what);
    return false;
}

static void clearSoundBank(WiiAudioSystem* wii) {
    free(wii->sondEntries);
    free(wii->audoEntries);
    if (wii->musEntries) {
        for (uint16_t i = 0; i < wii->musEntryCount; i++) free(wii->musEntries[i].name);
    }
    free(wii->musEntries);
    wii->sondEntries = NULL;
    wii->audoEntries = NULL;
    wii->musEntries = NULL;
    wii->sondEntryCount = 0;
    wii->audoEntryCount = 0;
    wii->musEntryCount = 0;
}

static void parseSoundBank(WiiAudioSystem* wii) {
    char* soundBankPath = wii->fileSystem->vtable->resolvePath(wii->fileSystem, "SOUNDBNK.BIN");
    FILE* f = fopen(soundBankPath, "rb");
    if (!f) {
        fprintf(stderr, "WiiAudioSystem: Could not open %s\n", soundBankPath);
        free(soundBankPath);
        return;
    }
    free(soundBankPath);

    clearSoundBank(wii);

    uint8_t header[7];
    if (!readExact(f, header, sizeof(header), "header")) goto fail;
    uint8_t version = header[0];
    wii->sondEntryCount = readLE16(&header[1]);
    wii->audoEntryCount = readLE16(&header[3]);
    wii->musEntryCount = readLE16(&header[5]);

    fprintf(stderr, "WiiAudioSystem: SOUNDBNK v%d, %d SOND, %d AUDO, %d MUS\n",
        version, wii->sondEntryCount, wii->audoEntryCount, wii->musEntryCount);

    wii->sondEntries = wii->sondEntryCount > 0
        ? safeCalloc(wii->sondEntryCount, sizeof(WiiSondEntry))
        : NULL;
    for (int i = 0; i < wii->sondEntryCount; i++) {
        uint8_t buf[12];
        if (!readExact(f, buf, sizeof(buf), "SOND entry")) goto fail;
        wii->sondEntries[i].audoIndex = readLE16(&buf[0]);
        wii->sondEntries[i].flags     = readLE32(&buf[2]);
        wii->sondEntries[i].volume    = readLES16(&buf[6]);
        wii->sondEntries[i].pitch     = readLES16(&buf[8]);
    }

    wii->audoEntries = wii->audoEntryCount > 0
        ? safeCalloc(wii->audoEntryCount, sizeof(WiiAudoEntry))
        : NULL;
    for (int i = 0; i < wii->audoEntryCount; i++) {
        uint8_t buf[20];
        if (!readExact(f, buf, sizeof(buf), "AUDO entry")) goto fail;
        wii->audoEntries[i].dataOffset    = readLE32(&buf[0]);
        wii->audoEntries[i].dataSize      = readLE32(&buf[4]);
        wii->audoEntries[i].sampleRate    = readLE16(&buf[8]);
        wii->audoEntries[i].channels      = buf[10];
        wii->audoEntries[i].bitsPerSample = buf[11];
        wii->audoEntries[i].format        = buf[12];
        wii->audoEntries[i].sampleCount   = readLE32(&buf[16]);   
    }

    wii->musEntries = wii->musEntryCount > 0
        ? safeCalloc(wii->musEntryCount, sizeof(WiiMusEntry))
        : NULL;
    for (int i = 0; i < wii->musEntryCount; i++) {
        uint8_t nameLen;
        if (!readExact(f, &nameLen, sizeof(nameLen), "MUS name length")) goto fail;
        char* name = safeMalloc(nameLen + 1);
        if (!readExact(f, name, nameLen, "MUS name")) {
            free(name);
            goto fail;
        }
        name[nameLen] = '\0';
        wii->musEntries[i].name = name;
    }
    for (int i = 0; i < wii->musEntryCount; i++) {
        uint8_t buf[16];
        if (!readExact(f, buf, sizeof(buf), "MUS entry")) goto fail;
        wii->musEntries[i].dataOffset  = readLE32(&buf[0]);
        wii->musEntries[i].dataSize    = readLE32(&buf[4]);
        wii->musEntries[i].sampleRate  = readLE16(&buf[8]);
        wii->musEntries[i].channels    = buf[10];
        wii->musEntries[i].format      = buf[11];
        wii->musEntries[i].sampleCount = readLE32(&buf[12]);
    }

    fclose(f);
    return;

fail:
    fclose(f);
    clearSoundBank(wii);
    fprintf(stderr, "WiiAudioSystem: invalid SOUNDBNK.BIN; banked audio disabled\n");
}

// ===[ Voice Helpers ]===

// Convert 0.0-1.0 gain to ASND volume (0-255)
static int gainToAsndVol(float gain) {
    int v = (int)(gain * 255.0f);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return v;
}

// Find a free ASND voice slot
static int findFreeVoice(WiiAudioSystem* wii) {
    bool used[WII_MAX_SOUND_INSTANCES] = {0};
    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (wii->instances[i].active) used[wii->instances[i].voice] = true;
    }
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        if (wii->musicStreams[i].active) used[wii->musicStreams[i].voice] = true;
    }
    for (int v = 0; v < WII_MAX_SOUND_INSTANCES; v++) {
        if (!used[v]) return v;
    }
    return -1;
}

static WiiSoundInstance* findSfxById(WiiAudioSystem* wii, int32_t instanceId) {
    int32_t idx = instanceId - WII_SOUND_INSTANCE_ID_BASE;
    if (idx < 0 || idx >= WII_MAX_SOUND_INSTANCES) return NULL;
    WiiSoundInstance* inst = &wii->instances[idx];
    if (!inst->active || inst->instanceId != instanceId) return NULL;
    return inst;
}

static WiiMusicStream* findMusicById(WiiAudioSystem* wii, int32_t instanceId) {
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        if (wii->musicStreams[i].active && wii->musicStreams[i].instanceId == instanceId)
            return &wii->musicStreams[i];
    }
    return NULL;
}

static bool ensureStreamBuffers(WiiMusicStream* stream) {
    for (int i = 0; i < 2; i++) {
        if (!stream->buffers[i]) {
            stream->buffers[i] = safeMemalign(32, WII_STREAM_BUFFER_BYTES);
            memset(stream->buffers[i], 0, WII_STREAM_BUFFER_BYTES);
        }
    }
    return stream->buffers[0] != NULL && stream->buffers[1] != NULL;
}

static void freeStreamBuffers(WiiMusicStream* stream) {
    for (int i = 0; i < 2; i++) {
        free(stream->buffers[i]);
        stream->buffers[i] = NULL;
    }
}

static uint32_t align32(uint32_t value) {
    return (value + 31u) & ~31u;
}

static bool decodeVorbisMemoryMono(const uint8_t* data, uint32_t dataSize, int16_t** outPcm,
    uint32_t* outPcmBytes, uint32_t* outSampleRate, uint32_t* outFrameCount) {
    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_memory(data, (int) dataSize, &error, NULL);
    if (!vorbis) {
        fprintf(stderr, "WiiAudioSystem: embedded Vorbis decode open failed (error %d)\n", error);
        return false;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    uint32_t frameCount = stb_vorbis_stream_length_in_samples(vorbis);
    if (frameCount == 0) {
        stb_vorbis_close(vorbis);
        return false;
    }

    uint32_t rawBytes = frameCount * sizeof(int16_t);
    uint32_t paddedBytes = align32(rawBytes);
    int16_t* pcm = safeMemalign(32, paddedBytes);
    memset(pcm, 0, paddedBytes);

    uint32_t written = 0;
    while (written < frameCount) {
        int got = stb_vorbis_get_samples_short_interleaved(
            vorbis,
            1,
            pcm + written,
            (int) (frameCount - written)
        );
        if (got <= 0) break;
        written += (uint32_t) got;
    }
    stb_vorbis_close(vorbis);

    if (written == 0) {
        free(pcm);
        return false;
    }

    *outPcm = pcm;
    *outPcmBytes = align32(written * sizeof(int16_t));
    *outSampleRate = info.sample_rate;
    *outFrameCount = written;
    return true;
}

static int streamFormatForChannels(uint8_t channels) {
    return channels > 1 ? VOICE_STEREO_16BIT : VOICE_MONO_16BIT;
}

static bool decodeVorbisFileInterleaved(const char* path, int16_t** outPcm,
    uint32_t* outPcmBytes, uint32_t* outSampleRate, uint32_t* outFrameCount, uint8_t* outChannels) {
    int error = 0;
    stb_vorbis* vorbis = stb_vorbis_open_filename(path, &error, NULL);
    if (!vorbis) {
        fprintf(stderr, "WiiAudioSystem: external Vorbis decode open failed for '%s' (error %d)\n", path, error);
        return false;
    }

    stb_vorbis_info info = stb_vorbis_get_info(vorbis);
    uint32_t frameCount = stb_vorbis_stream_length_in_samples(vorbis);
    uint8_t channels = info.channels > 1 ? 2 : 1;
    if (frameCount == 0 || info.sample_rate == 0) {
        stb_vorbis_close(vorbis);
        return false;
    }

    uint32_t rawBytes = frameCount * channels * sizeof(int16_t);
    uint32_t paddedBytes = align32(rawBytes);
    int16_t* pcm = safeMemalign(32, paddedBytes);
    memset(pcm, 0, paddedBytes);

    uint32_t written = 0;
    while (written < frameCount) {
        int got = stb_vorbis_get_samples_short_interleaved(
            vorbis,
            channels,
            pcm + (written * channels),
            (int)((frameCount - written) * channels)
        );
        if (got <= 0) break;
        written += (uint32_t)got;
    }
    stb_vorbis_close(vorbis);

    if (written == 0) {
        free(pcm);
        return false;
    }

    *outPcm = pcm;
    *outPcmBytes = align32(written * channels * sizeof(int16_t));
    *outSampleRate = info.sample_rate;
    *outFrameCount = written;
    *outChannels = channels;
    return true;
}

static bool wiiTryResolveExistingAudioPath(WiiAudioSystem* wii, const char* relativePath, char** outPath) {
    if (!wii->fileSystem || !relativePath || relativePath[0] == '\0') return false;

    char* resolved = wii->fileSystem->vtable->resolvePath(wii->fileSystem, relativePath);
    if (!resolved) return false;

    FILE* f = fopen(resolved, "rb");
    if (!f) {
        free(resolved);
        return false;
    }

    fclose(f);
    *outPath = resolved;
    return true;
}

static bool wiiResolveExternalSoundPath(WiiAudioSystem* wii, Sound* sound, char** outPath) {
    const char* names[2] = {
        sound && sound->file && sound->file[0] ? sound->file : NULL,
        sound && sound->name && sound->name[0] ? sound->name : NULL
    };
    const char* prefixes[] = { "", "snd/", "sfx/", "sounds/" };

    for (uint32_t nameIdx = 0; nameIdx < sizeof(names) / sizeof(names[0]); nameIdx++) {
        const char* name = names[nameIdx];
        if (!name) continue;

        bool hasExtension = strchr(name, '.') != NULL;
        for (uint32_t prefixIdx = 0; prefixIdx < sizeof(prefixes) / sizeof(prefixes[0]); prefixIdx++) {
            char candidate[512];
            snprintf(candidate, sizeof(candidate), "%s%s", prefixes[prefixIdx], name);
            if (wiiTryResolveExistingAudioPath(wii, candidate, outPath)) return true;

            if (!hasExtension) {
                snprintf(candidate, sizeof(candidate), "%s%s.ogg", prefixes[prefixIdx], name);
                if (wiiTryResolveExistingAudioPath(wii, candidate, outPath)) return true;
            }
        }
    }

    return false;
}

static int32_t wiiPlayExternalSound(WiiAudioSystem* wii, int32_t soundIndex, int32_t priority, bool loop) {
    if (arrlen(wii->base.audioGroups) == 0 || wii->base.audioGroups[0] == NULL) return -1;
    DataWin* dw = wii->base.audioGroups[0];
    if (soundIndex < 0 || (uint32_t)soundIndex >= dw->sond.count) return -1;

    Sound* sound = &dw->sond.sounds[soundIndex];
    char* path = NULL;
    if (!wiiResolveExternalSoundPath(wii, sound, &path)) return -1;

    int16_t* pcmData = NULL;
    uint32_t pcmBytes = 0;
    uint32_t sampleRate = 0;
    uint32_t frameCount = 0;
    uint8_t channels = 1;
    if (!decodeVorbisFileInterleaved(path, &pcmData, &pcmBytes, &sampleRate, &frameCount, &channels)) {
        fprintf(stderr, "WiiAudioSystem: failed to decode external sound '%s' (%s)\n",
            sound->name ? sound->name : "(unnamed)", path);
        free(path);
        return -1;
    }

    WiiSoundInstance* slot = NULL;
    int slotIdx = -1;
    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (!wii->instances[i].active) { slot = &wii->instances[i]; slotIdx = i; break; }
    }
    if (!slot) {
        free(pcmData);
        free(path);
        return -1;
    }

    int voice = findFreeVoice(wii);
    if (voice < 0) {
        free(pcmData);
        free(path);
        return -1;
    }

    float volume = sound->volume;
    float pitch = sound->pitch == 0.0f ? 1.0f : sound->pitch;
    uint32_t pitchedRate = (uint32_t)((float)sampleRate * pitch);
    int vol = gainToAsndVol(volume * wii->masterGain);
    DCFlushRange(pcmData, pcmBytes);

    s32 result;
    if (loop) {
        result = ASND_SetInfiniteVoice(voice, streamFormatForChannels(channels), pitchedRate, 0, pcmData, pcmBytes, vol, vol);
    } else {
        result = ASND_SetVoice(voice, streamFormatForChannels(channels), pitchedRate, 0, pcmData, pcmBytes, vol, vol, NULL);
    }
    if (result != SND_OK) {
        fprintf(stderr, "WiiAudioSystem: ASND_SetVoice failed for external sound '%s' (error %d)\n",
            sound->name ? sound->name : "(unnamed)", result);
        free(pcmData);
        free(path);
        return -1;
    }

    int32_t instanceId = WII_SOUND_INSTANCE_ID_BASE + slotIdx;
    memset(slot, 0, sizeof(WiiSoundInstance));
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->audoIndex = -1;
    slot->instanceId = instanceId;
    slot->priority = priority;
    slot->loop = loop;
    slot->voice = voice;
    slot->pitch = pitch;
    slot->sondPitch = 1.0f;
    slot->currentGain = volume;
    slot->targetGain = volume;
    slot->startGain = volume;
    slot->sondVolume = 1.0f;
    slot->sampleRate = sampleRate;
    slot->channels = channels;
    slot->pcmData = pcmData;
    slot->pcmBytes = pcmBytes;
    wii->nextInstanceCounter++;

    fprintf(stderr, "WiiAudioSystem: external SFX '%s' -> '%s' (%u Hz, %u ch)\n",
        sound->name ? sound->name : "(unnamed)", path, sampleRate, channels);
    free(path);
    (void)frameCount;
    return instanceId;
}

static uint32_t streamBufferRawBytes(WiiMusicStream* stream, int bufIdx) {
    uint32_t channels = stream->channels > 1 ? 2u : 1u;
    return stream->bufferFrameCount[bufIdx] * channels * sizeof(int16_t);
}

static uint32_t streamBufferSubmitBytes(WiiMusicStream* stream, int bufIdx) {
    uint32_t rawBytes = streamBufferRawBytes(stream, bufIdx);
    uint32_t paddedBytes = align32(rawBytes);
    uint32_t capacity = WII_STREAM_BUFFER_BYTES;
    if (paddedBytes > capacity) paddedBytes = capacity;
    if (paddedBytes > rawBytes) {
        memset(((uint8_t*) stream->buffers[bufIdx]) + rawBytes, 0, paddedBytes - rawBytes);
    }
    return paddedBytes;
}

// ===[ Streaming ]===

static void streamFillBuffer(WiiAudioSystem* wii, WiiMusicStream* stream, int bufIdx) {
    if (stream->isOggStream) {
        stream->bufferFrameCount[bufIdx] = 0;
        if (!stream->vorbis) {
            stream->endOfTrack = true;
            return;
        }

        int channels = stream->channels > 1 ? 2 : 1;
        memset(stream->buffers[bufIdx], 0, WII_STREAM_BUFFER_BYTES);
        int frames = stb_vorbis_get_samples_short_interleaved(
            (stb_vorbis*) stream->vorbis,
            channels,
            stream->buffers[bufIdx],
            WII_STREAM_CHUNK_FRAMES * channels
        );

        if (frames == 0 && stream->loop) {
            stb_vorbis_seek_start((stb_vorbis*) stream->vorbis);
            frames = stb_vorbis_get_samples_short_interleaved(
                (stb_vorbis*) stream->vorbis,
                channels,
                stream->buffers[bufIdx],
                WII_STREAM_CHUNK_FRAMES * channels
            );
        }

        if (frames <= 0) {
            stream->endOfTrack = true;
            return;
        }

        stream->bufferFrameCount[bufIdx] = (uint32_t) frames;
        return;
    }

    if (stream->fileOffset >= stream->fileEndOffset) {
        stream->bufferFrameCount[bufIdx] = 0;
        return;
    }
    uint32_t remaining = stream->fileEndOffset - stream->fileOffset;
    uint32_t toRead = WII_STREAM_ADPCM_CHUNK_BYTES;
    if (toRead > remaining) toRead = remaining;

    uint8_t adpcmBuf[WII_STREAM_ADPCM_CHUNK_BYTES];
    fseek(wii->soundsFile, (long) stream->fileOffset, SEEK_SET);
    size_t bytesRead = fread(adpcmBuf, 1, toRead, wii->soundsFile);
    stream->fileOffset += (uint32_t) bytesRead;

    uint32_t samples = imaAdpcmDecodeBlock(adpcmBuf, (uint32_t) bytesRead,
        stream->buffers[bufIdx], &stream->decoderPredictor, &stream->decoderStepIndex);
    stream->bufferFrameCount[bufIdx] = samples;

    if (stream->fileOffset >= stream->fileEndOffset)
        stream->endOfTrack = true;
}

static bool wiiEnsureAudioGroupLoaded(WiiAudioSystem* wii, int32_t groupIndex) {
#if !WII_ENABLE_EMBEDDED_AUDO_FALLBACK
    static bool logged = false;
    if (!logged) {
        fprintf(stderr, "WiiAudioSystem: embedded AUDO fallback disabled; use SOUNDBNK.BIN/SOUNDS.BIN or external streams\n");
        logged = true;
    }
    (void)wii;
    (void)groupIndex;
    return false;
#else
    if (groupIndex < 0) return false;

    while (arrlen(wii->base.audioGroups) <= groupIndex) {
        arrput(wii->base.audioGroups, NULL);
    }

    if (wii->base.audioGroups[groupIndex] != NULL) return true;
    if (groupIndex == 0 || !wii->fileSystem) return false;

    char filename[64];
    snprintf(filename, sizeof(filename), "audiogroup%d.dat", groupIndex);
    char* path = wii->fileSystem->vtable->resolvePath(wii->fileSystem, filename);
    if (!path) return false;

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "WiiAudioSystem: audio group %d missing (%s)\n", groupIndex, path);
        free(path);
        return false;
    }
    fclose(f);

    DataWin* audioGroup = DataWin_parse(path, (DataWinParserOptions) { .parseAudo = true });
    free(path);
    wii->base.audioGroups[groupIndex] = audioGroup;
    fprintf(stderr, "WiiAudioSystem: loaded audio group %d (%u AUDO entries)\n",
        groupIndex, audioGroup->audo.count);
    return true;
#endif
}

static int32_t wiiPlayEmbeddedSound(WiiAudioSystem* wii, int32_t soundIndex, int32_t priority, bool loop) {
#if !WII_ENABLE_EMBEDDED_AUDO_FALLBACK
    (void)wii;
    (void)soundIndex;
    (void)priority;
    (void)loop;
    return -1;
#else
    if (arrlen(wii->base.audioGroups) == 0 || wii->base.audioGroups[0] == NULL) return -1;
    DataWin* mainDw = wii->base.audioGroups[0];
    if (soundIndex < 0 || (uint32_t) soundIndex >= mainDw->sond.count) return -1;

    Sound* sound = &mainDw->sond.sounds[soundIndex];
    if (sound->audioFile < 0) return -1;
    if (!wiiEnsureAudioGroupLoaded(wii, sound->audioGroup)) return -1;

    DataWin* audioGroup = wii->base.audioGroups[sound->audioGroup];
    if (!audioGroup || (uint32_t) sound->audioFile >= audioGroup->audo.count) {
        fprintf(stderr, "WiiAudioSystem: invalid AUDO index %d for sound '%s'\n",
            sound->audioFile, sound->name ? sound->name : "(unnamed)");
        return -1;
    }

    AudioEntry* entry = &audioGroup->audo.entries[sound->audioFile];
    if (!entry->data || entry->dataSize == 0) return -1;

    int16_t* pcmData = NULL;
    uint32_t pcmBytes = 0;
    uint32_t sampleRate = 0;
    uint32_t frameCount = 0;
    if (!decodeVorbisMemoryMono(entry->data, entry->dataSize, &pcmData, &pcmBytes, &sampleRate, &frameCount)) {
        fprintf(stderr, "WiiAudioSystem: failed to decode embedded sound '%s'\n",
            sound->name ? sound->name : "(unnamed)");
        return -1;
    }

    WiiSoundInstance* slot = NULL;
    int slotIdx = -1;
    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (!wii->instances[i].active) { slot = &wii->instances[i]; slotIdx = i; break; }
    }
    if (!slot) {
        free(pcmData);
        return -1;
    }

    int voice = findFreeVoice(wii);
    if (voice < 0) {
        free(pcmData);
        return -1;
    }

    float volume = sound->volume;
    float pitch = sound->pitch == 0.0f ? 1.0f : sound->pitch;
    uint32_t pitchedRate = (uint32_t) ((float) sampleRate * pitch);
    int vol = gainToAsndVol(volume * wii->masterGain);
    DCFlushRange(pcmData, pcmBytes);

    s32 result;
    if (loop) {
        result = ASND_SetInfiniteVoice(voice, VOICE_MONO_16BIT, pitchedRate, 0, pcmData, pcmBytes, vol, vol);
    } else {
        result = ASND_SetVoice(voice, VOICE_MONO_16BIT, pitchedRate, 0, pcmData, pcmBytes, vol, vol, NULL);
    }
    if (result != SND_OK) {
        fprintf(stderr, "WiiAudioSystem: ASND_SetVoice failed for embedded sound '%s' (error %d)\n",
            sound->name ? sound->name : "(unnamed)", result);
        free(pcmData);
        return -1;
    }

    int32_t instanceId = WII_SOUND_INSTANCE_ID_BASE + slotIdx;
    memset(slot, 0, sizeof(WiiSoundInstance));
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->audoIndex = sound->audioFile;
    slot->instanceId = instanceId;
    slot->priority = priority;
    slot->loop = loop;
    slot->voice = voice;
    slot->pitch = pitch;
    slot->sondPitch = 1.0f;
    slot->currentGain = volume;
    slot->targetGain = volume;
    slot->startGain = volume;
    slot->sondVolume = 1.0f;
    slot->sampleRate = sampleRate;
    slot->channels = 1;
    slot->pcmData = pcmData;
    slot->pcmBytes = pcmBytes;
    wii->nextInstanceCounter++;
    (void) frameCount;
    return instanceId;
#endif
}

// ===[ Vtable Implementations ]===

static void wiiInit(AudioSystem* audio, DataWin* dataWin, FileSystem* fileSystem) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;

    wii->dataWin = dataWin;
    wii->fileSystem = fileSystem;
    memset(wii->instances, 0, sizeof(wii->instances));
    memset(wii->musicStreams, 0, sizeof(wii->musicStreams));
    memset(wii->externalStreams, 0, sizeof(wii->externalStreams));
    wii->nextInstanceCounter = 0;
    wii->masterGain = 1.0f;
    g_wiiAudio = wii;
    arrput(wii->base.audioGroups, dataWin);

    parseSoundBank(wii);
    if (wii->sondEntries && wii->audoEntries) {
        char* soundsPath = wii->fileSystem->vtable->resolvePath(wii->fileSystem, "SOUNDS.BIN");
        wii->soundsFile = fopen(soundsPath, "rb");
        if (!wii->soundsFile) {
            fprintf(stderr, "WiiAudioSystem: Failed to open %s; banked audio disabled\n", soundsPath);
        } else {
            wii->bankAvailable = true;
            fprintf(stderr, "WiiAudioSystem: banked audio ready\n");
        }
        free(soundsPath);
    } else {
        fprintf(stderr, "WiiAudioSystem: SOUNDBNK.BIN missing; using external OGG streams only\n");
    }

    wii->initialized = true;

    fprintf(stderr, "WiiAudioSystem: Initialized\n");
}

static void wiiDestroy(AudioSystem* audio) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;

    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (wii->instances[i].active) {
            ASND_StopVoice(wii->instances[i].voice);
            free(wii->instances[i].pcmData);
        }
    }
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        if (wii->musicStreams[i].active) {
            ASND_StopVoice(wii->musicStreams[i].voice);
            if (wii->musicStreams[i].vorbis)
                stb_vorbis_close((stb_vorbis*) wii->musicStreams[i].vorbis);
        }
        freeStreamBuffers(&wii->musicStreams[i]);
    }
    for (int i = 0; i < WII_MAX_EXTERNAL_STREAMS; i++) {
        free(wii->externalStreams[i].name);
        free(wii->externalStreams[i].filePath);
    }
    for (int i = 1; i < arrlen(wii->base.audioGroups); i++) {
        DataWin_free(wii->base.audioGroups[i]);
    }
    arrfree(wii->base.audioGroups);

    if (wii->soundsFile) fclose(wii->soundsFile);
    free(wii->sondEntries);
    free(wii->audoEntries);
    for (int i = 0; i < wii->musEntryCount; i++) free(wii->musEntries[i].name);
    free(wii->musEntries);
    if (g_wiiAudio == wii) g_wiiAudio = NULL;
    free(wii);
}

static void wiiUpdate(AudioSystem* audio, float deltaTime) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (!wii->initialized) return;
    if (deltaTime > 0.1f) deltaTime = 0.1f;

    // Update SFX gain fading
    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        WiiSoundInstance* inst = &wii->instances[i];
        if (!inst->active) continue;

        // Check if ASND finished playing
        if (ASND_StatusVoice(inst->voice) == SND_UNUSED) {
            if (!inst->loop) {
                inst->active = false;
                free(inst->pcmData);
                inst->pcmData = NULL;
                continue;
            }
        }

        if (inst->fadeTimeRemaining > 0.0f) {
            inst->fadeTimeRemaining -= deltaTime;
            if (inst->fadeTimeRemaining <= 0.0f) {
                inst->fadeTimeRemaining = 0.0f;
                inst->currentGain = inst->targetGain;
            } else {
                float t = 1.0f - (inst->fadeTimeRemaining / inst->fadeTotalTime);
                inst->currentGain = inst->startGain + (inst->targetGain - inst->startGain) * t;
            }
            float gain = inst->currentGain * inst->sondVolume * wii->masterGain;
            int vol = gainToAsndVol(gain);
            ASND_ChangeVolumeVoice(inst->voice, vol, vol);
        }
    }

    // Update music stream gain fading and refill buffers
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        WiiMusicStream* stream = &wii->musicStreams[i];
        if (!stream->active) continue;

        if (stream->fadeTimeRemaining > 0.0f) {
            stream->fadeTimeRemaining -= deltaTime;
            if (stream->fadeTimeRemaining <= 0.0f) {
                stream->fadeTimeRemaining = 0.0f;
                stream->currentGain = stream->targetGain;
            } else {
                float t = 1.0f - (stream->fadeTimeRemaining / stream->fadeTotalTime);
                stream->currentGain = stream->startGain + (stream->targetGain - stream->startGain) * t;
            }
            float gain = stream->currentGain * stream->sondVolume * wii->masterGain;
            int vol = gainToAsndVol(gain);
            ASND_ChangeVolumeVoice(stream->voice, vol, vol);
        }

        // Feed next buffer chunk to ASND if it is ready for more.
        if (stream->needsRefill || ASND_TestVoiceBufferReady(stream->voice) == 1) {
            int backBuf = stream->activeBuffer ^ 1;
            streamFillBuffer(wii, stream, backBuf);
            uint32_t bytes = streamBufferSubmitBytes(stream, backBuf);
            if (bytes > 0) {
                // DCFlush required before feeding to ASND
                DCFlushRange(stream->buffers[backBuf], bytes);
                s32 addResult = ASND_AddVoice(stream->voice, stream->buffers[backBuf], bytes);
                if (addResult == SND_OK) {
                    stream->activeBuffer = backBuf;
                    stream->needsRefill = false;
                } else {
                    stream->needsRefill = true;
                }
            } else if (stream->endOfTrack) {
                if (stream->loop) {
                    // Reset and refill
                    if (stream->isOggStream && stream->vorbis) {
                        stb_vorbis_seek_start((stb_vorbis*) stream->vorbis);
                    } else {
                        stream->fileOffset = stream->fileStartOffset;
                        stream->decoderPredictor = 0;
                        stream->decoderStepIndex = 0;
                    }
                    stream->endOfTrack = false;
                    streamFillBuffer(wii, stream, backBuf);
                    bytes = streamBufferSubmitBytes(stream, backBuf);
                    if (bytes > 0) {
                        DCFlushRange(stream->buffers[backBuf], bytes);
                        s32 addResult = ASND_AddVoice(stream->voice, stream->buffers[backBuf], bytes);
                        if (addResult == SND_OK) {
                            stream->activeBuffer = backBuf;
                            stream->needsRefill = false;
                        } else {
                            stream->needsRefill = true;
                        }
                    }
                } else {
                    stream->active = false;
                    ASND_StopVoice(stream->voice);
                    if (stream->vorbis) {
                        stb_vorbis_close((stb_vorbis*) stream->vorbis);
                        stream->vorbis = NULL;
                    }
                }
            }
        }
    }
}

static int32_t wiiPlaySound(AudioSystem* audio, int32_t soundIndex, int32_t priority, bool loop) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (!wii->initialized) return -1;

    // MUS stream path
    if (soundIndex >= WII_AUDIO_STREAM_INDEX_BASE) {
        int32_t streamIndex = soundIndex - WII_AUDIO_STREAM_INDEX_BASE;
        bool isBankStream = streamIndex >= 0 && streamIndex < wii->musEntryCount;
        bool isExternalStream = !isBankStream &&
            streamIndex >= wii->musEntryCount &&
            streamIndex < wii->musEntryCount + WII_MAX_EXTERNAL_STREAMS &&
            wii->externalStreams[streamIndex - wii->musEntryCount].active;

        if (!isBankStream && !isExternalStream) return -1;

        int voice = findFreeVoice(wii);
        if (voice < 0) return -1;

        WiiMusicStream* stream = NULL;
        int slot = -1;
        for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
            if (!wii->musicStreams[i].active) { stream = &wii->musicStreams[i]; slot = i; break; }
        }
        if (!stream) return -1;

        int32_t instanceId = WII_SOUND_INSTANCE_ID_BASE + WII_MAX_SOUND_INSTANCES + slot;
        int16_t* existingBuffers[2] = { stream->buffers[0], stream->buffers[1] };
        memset(stream, 0, sizeof(WiiMusicStream));
        stream->buffers[0] = existingBuffers[0];
        stream->buffers[1] = existingBuffers[1];
        if (!ensureStreamBuffers(stream)) return -1;

        stream->active = true;
        stream->soundIndex = soundIndex;
        stream->audoIndex = -1;
        stream->instanceId = instanceId;
        stream->priority = priority;
        stream->loop = loop;
        stream->voice = voice;
        stream->channels = 1;
        stream->currentGain = 1.0f;
        stream->targetGain = 1.0f;
        stream->startGain = 1.0f;
        stream->sondVolume = 1.0f;
        stream->pitch = 1.0f;
        stream->sondPitch = 1.0f;

        uint32_t sampleRate = WII_OUTPUT_FREQ;
        if (isExternalStream) {
            WiiExternalStreamEntry* ext = &wii->externalStreams[streamIndex - wii->musEntryCount];
            int error = 0;
            stb_vorbis* vorbis = stb_vorbis_open_filename(ext->filePath, &error, NULL);
            if (!vorbis) {
                fprintf(stderr, "WiiAudioSystem: failed to open OGG stream '%s' (%s, error %d)\n",
                    ext->name ? ext->name : "(unnamed)", ext->filePath, error);
                stream->active = false;
                return -1;
            }

            stb_vorbis_info info = stb_vorbis_get_info(vorbis);
            stream->isOggStream = true;
            stream->vorbis = vorbis;
            stream->sampleRate = info.sample_rate;
            stream->channels = info.channels > 1 ? 2 : 1;
            stream->lengthSeconds = stb_vorbis_stream_length_in_seconds(vorbis);
            sampleRate = info.sample_rate;
            fprintf(stderr, "WiiAudioSystem: OGG stream '%s' %u Hz %d ch\n",
                ext->name ? ext->name : "(unnamed)", info.sample_rate, info.channels);
        } else {
            WiiMusEntry* mus = &wii->musEntries[streamIndex];
            if (!wii->soundsFile) {
                stream->active = false;
                return -1;
            }
            stream->isOggStream = false;
            stream->sampleRate = mus->sampleRate;
            stream->channels = 1;
            stream->fileStartOffset = mus->dataOffset;
            stream->fileEndOffset = mus->dataOffset + mus->dataSize;
            stream->fileOffset = mus->dataOffset;
            stream->lengthSeconds = mus->sampleRate == 0 ? 0.0f : (float) mus->sampleCount / (float) mus->sampleRate;
            sampleRate = mus->sampleRate;
        }

        streamFillBuffer(wii, stream, 0);
        streamFillBuffer(wii, stream, 1);
        stream->activeBuffer = 0;
        stream->needsRefill = false;

        int vol = gainToAsndVol(wii->masterGain);
        uint32_t bytes = streamBufferSubmitBytes(stream, 0);
        if (bytes == 0) {
            if (stream->vorbis) stb_vorbis_close((stb_vorbis*) stream->vorbis);
            stream->active = false;
            return -1;
        }
        DCFlushRange(stream->buffers[0], bytes);
        s32 result = ASND_SetVoice(voice, streamFormatForChannels(stream->channels), sampleRate, 0,
            stream->buffers[0], bytes,
            vol, vol, streamVoiceCallback);

        if (result != SND_OK) {
            fprintf(stderr, "WiiAudioSystem: ASND_SetVoice failed for stream %d (error %d)\n", soundIndex, result);
            if (stream->vorbis) stb_vorbis_close((stb_vorbis*) stream->vorbis);
            stream->active = false;
            return -1;
        }

        uint32_t queuedBytes = streamBufferSubmitBytes(stream, 1);
        if (queuedBytes > 0) {
            DCFlushRange(stream->buffers[1], queuedBytes);
            if (ASND_AddVoice(voice, stream->buffers[1], queuedBytes) == SND_OK) {
                stream->activeBuffer = 1;
            } else {
                stream->needsRefill = true;
            }
        }

        return instanceId;
    }

    // SOND path
    if (!wii->bankAvailable || !wii->soundsFile) {
        int32_t externalId = wiiPlayExternalSound(wii, soundIndex, priority, loop);
        if (externalId >= 0) return externalId;
        return wiiPlayEmbeddedSound(wii, soundIndex, priority, loop);
    }
    if (soundIndex < 0 || soundIndex >= wii->sondEntryCount) {
        int32_t externalId = wiiPlayExternalSound(wii, soundIndex, priority, loop);
        if (externalId >= 0) return externalId;
        return wiiPlayEmbeddedSound(wii, soundIndex, priority, loop);
    }
    WiiSondEntry* sond = &wii->sondEntries[soundIndex];
    if (sond->audoIndex == 0xFFFF || sond->audoIndex >= wii->audoEntryCount) {
        int32_t externalId = wiiPlayExternalSound(wii, soundIndex, priority, loop);
        if (externalId >= 0) return externalId;
        return wiiPlayEmbeddedSound(wii, soundIndex, priority, loop);
    }

    WiiAudoEntry* audo = &wii->audoEntries[sond->audoIndex];
    float sondVolume = (float) sond->volume / 256.0f;
    float sondPitch  = (sond->pitch == 0) ? 1.0f : (float) sond->pitch / 256.0f;

    // Find free SFX slot
    WiiSoundInstance* slot = NULL;
    int slotIdx = -1;
    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (!wii->instances[i].active) { slot = &wii->instances[i]; slotIdx = i; break; }
    }
    if (!slot) return -1;

    int voice = findFreeVoice(wii);
    if (voice < 0) return -1;

    // Decode ADPCM
    uint32_t pcmSamples = audo->dataSize * 2;
    uint32_t pcmBytes = pcmSamples * sizeof(int16_t);
    int16_t* pcmData = safeMemalign(32, pcmBytes);

    uint8_t* adpcmBuf = safeMalloc(audo->dataSize);
    fseek(wii->soundsFile, (long) audo->dataOffset, SEEK_SET);
    fread(adpcmBuf, 1, audo->dataSize, wii->soundsFile);
    imaAdpcmDecode(adpcmBuf, audo->dataSize, pcmData);
    free(adpcmBuf);

    int32_t instanceId = WII_SOUND_INSTANCE_ID_BASE + slotIdx;
    slot->active = true;
    slot->soundIndex = soundIndex;
    slot->audoIndex = sond->audoIndex;
    slot->instanceId = instanceId;
    slot->priority = priority;
    slot->loop = loop;
    slot->voice = voice;
    slot->pitch = 1.0f;
    slot->sondPitch = sondPitch;
    slot->currentGain = sondVolume;
    slot->targetGain = sondVolume;
    slot->startGain = sondVolume;
    slot->sondVolume = sondVolume;
    slot->sampleRate = audo->sampleRate;
    slot->channels = 1;
    slot->fadeTimeRemaining = 0.0f;
    slot->fadeTotalTime = 0.0f;
    slot->pcmData = pcmData;
    slot->pcmBytes = pcmBytes;

    float gain = sondVolume * wii->masterGain;
    int vol = gainToAsndVol(gain);
    uint32_t sampleRate = (uint32_t)((float) audo->sampleRate * sondPitch);

    DCFlushRange(pcmData, pcmBytes);

    s32 result = ASND_SetVoice(
        voice,
        VOICE_MONO_16BIT,
        sampleRate,
        0,
        pcmData,
        pcmBytes,
        vol,
        vol,
        NULL
    );
    if (result != SND_OK) {
        fprintf(stderr, "WiiAudioSystem: ASND_SetVoice failed for sound %d (error %d)\n", soundIndex, result);
        free(slot->pcmData);
        memset(slot, 0, sizeof(WiiSoundInstance));
        return -1;
    }

    wii->nextInstanceCounter++;
    return instanceId;
}

static void wiiStopSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        WiiSoundInstance* sfx = findSfxById(wii, soundOrInstance);
        if (sfx) { ASND_StopVoice(sfx->voice); free(sfx->pcmData); sfx->pcmData = NULL; sfx->active = false; return; }
        WiiMusicStream* mus = findMusicById(wii, soundOrInstance);
        if (mus) {
            ASND_StopVoice(mus->voice);
            if (mus->vorbis) {
                stb_vorbis_close((stb_vorbis*) mus->vorbis);
                mus->vorbis = NULL;
            }
            mus->active = false;
        }
    } else {
        for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
            if (wii->instances[i].active && wii->instances[i].soundIndex == soundOrInstance) {
                ASND_StopVoice(wii->instances[i].voice);
                free(wii->instances[i].pcmData);
                wii->instances[i].pcmData = NULL;
                wii->instances[i].active = false;
            }
        }
        for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
            if (wii->musicStreams[i].active && wii->musicStreams[i].soundIndex == soundOrInstance) {
                ASND_StopVoice(wii->musicStreams[i].voice);
                if (wii->musicStreams[i].vorbis) {
                    stb_vorbis_close((stb_vorbis*) wii->musicStreams[i].vorbis);
                    wii->musicStreams[i].vorbis = NULL;
                }
                wii->musicStreams[i].active = false;
            }
        }
    }
}

static void wiiStopAll(AudioSystem* audio) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (wii->instances[i].active) {
            ASND_StopVoice(wii->instances[i].voice);
            free(wii->instances[i].pcmData);
            wii->instances[i].pcmData = NULL;
            wii->instances[i].active = false;
        }
    }
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        if (wii->musicStreams[i].active) {
            ASND_StopVoice(wii->musicStreams[i].voice);
            if (wii->musicStreams[i].vorbis) {
                stb_vorbis_close((stb_vorbis*) wii->musicStreams[i].vorbis);
                wii->musicStreams[i].vorbis = NULL;
            }
            wii->musicStreams[i].active = false;
        }
    }
}

static bool wiiIsPlaying(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        WiiSoundInstance* sfx = findSfxById(wii, soundOrInstance);
        if (sfx) return !sfx->paused && ASND_StatusVoice(sfx->voice) != SND_UNUSED;
        WiiMusicStream* mus = findMusicById(wii, soundOrInstance);
        if (mus) return !mus->paused;
        return false;
    }
    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (wii->instances[i].active && wii->instances[i].soundIndex == soundOrInstance && !wii->instances[i].paused) return true;
    }
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        if (wii->musicStreams[i].active && wii->musicStreams[i].soundIndex == soundOrInstance && !wii->musicStreams[i].paused) return true;
    }
    return false;
}

static void wiiPauseSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        WiiSoundInstance* sfx = findSfxById(wii, soundOrInstance);
        if (sfx) { sfx->paused = true; ASND_PauseVoice(sfx->voice, 1); return; }
        WiiMusicStream* mus = findMusicById(wii, soundOrInstance);
        if (mus) { mus->paused = true; ASND_PauseVoice(mus->voice, 1); }
    } else {
        for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
            if (wii->instances[i].active && wii->instances[i].soundIndex == soundOrInstance) {
                wii->instances[i].paused = true; ASND_PauseVoice(wii->instances[i].voice, 1);
            }
        }
        for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
            if (wii->musicStreams[i].active && wii->musicStreams[i].soundIndex == soundOrInstance) {
                wii->musicStreams[i].paused = true; ASND_PauseVoice(wii->musicStreams[i].voice, 1);
            }
        }
    }
}

static void wiiResumeSound(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        WiiSoundInstance* sfx = findSfxById(wii, soundOrInstance);
        if (sfx) { sfx->paused = false; ASND_PauseVoice(sfx->voice, 0); return; }
        WiiMusicStream* mus = findMusicById(wii, soundOrInstance);
        if (mus) { mus->paused = false; ASND_PauseVoice(mus->voice, 0); }
    } else {
        for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
            if (wii->instances[i].active && wii->instances[i].soundIndex == soundOrInstance) {
                wii->instances[i].paused = false; ASND_PauseVoice(wii->instances[i].voice, 0);
            }
        }
        for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
            if (wii->musicStreams[i].active && wii->musicStreams[i].soundIndex == soundOrInstance) {
                wii->musicStreams[i].paused = false; ASND_PauseVoice(wii->musicStreams[i].voice, 0);
            }
        }
    }
}

static void wiiPauseAll(AudioSystem* audio) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (wii->instances[i].active) { wii->instances[i].paused = true; ASND_PauseVoice(wii->instances[i].voice, 1); }
    }
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        if (wii->musicStreams[i].active) { wii->musicStreams[i].paused = true; ASND_PauseVoice(wii->musicStreams[i].voice, 1); }
    }
}

static void wiiResumeAll(AudioSystem* audio) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (wii->instances[i].active) { wii->instances[i].paused = false; ASND_PauseVoice(wii->instances[i].voice, 0); }
    }
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        if (wii->musicStreams[i].active) { wii->musicStreams[i].paused = false; ASND_PauseVoice(wii->musicStreams[i].voice, 0); }
    }
}

static void wiiSetSoundGain(AudioSystem* audio, int32_t soundOrInstance, float gain, uint32_t timeMs) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        WiiSoundInstance* sfx = findSfxById(wii, soundOrInstance);
        if (sfx) {
            if (timeMs == 0) { sfx->currentGain = gain; sfx->targetGain = gain; sfx->fadeTimeRemaining = 0.0f; }
            else { sfx->startGain = sfx->currentGain; sfx->targetGain = gain; sfx->fadeTotalTime = timeMs / 1000.0f; sfx->fadeTimeRemaining = sfx->fadeTotalTime; }
            int vol = gainToAsndVol(sfx->currentGain * sfx->sondVolume * wii->masterGain);
            ASND_ChangeVolumeVoice(sfx->voice, vol, vol);
            return;
        }
        WiiMusicStream* mus = findMusicById(wii, soundOrInstance);
        if (mus) {
            if (timeMs == 0) { mus->currentGain = gain; mus->targetGain = gain; mus->fadeTimeRemaining = 0.0f; }
            else { mus->startGain = mus->currentGain; mus->targetGain = gain; mus->fadeTotalTime = timeMs / 1000.0f; mus->fadeTimeRemaining = mus->fadeTotalTime; }
            int vol = gainToAsndVol(mus->currentGain * mus->sondVolume * wii->masterGain);
            ASND_ChangeVolumeVoice(mus->voice, vol, vol);
        }
    }
}

static float wiiGetSoundGain(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        WiiSoundInstance* sfx = findSfxById(wii, soundOrInstance);
        if (sfx) return sfx->currentGain;
        WiiMusicStream* mus = findMusicById(wii, soundOrInstance);
        if (mus) return mus->currentGain;
    }
    return 0.0f;
}

static void wiiSetSoundPitch(AudioSystem* audio, int32_t soundOrInstance, float pitch) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        WiiSoundInstance* sfx = findSfxById(wii, soundOrInstance);
        if (sfx) {
            sfx->pitch = pitch;
            // Re-set voice with new sample rate to apply pitch
            uint32_t sr = (uint32_t)((float)sfx->sampleRate * pitch * sfx->sondPitch);
            ASND_StopVoice(sfx->voice);
            DCFlushRange(sfx->pcmData, sfx->pcmBytes);
            int vol = gainToAsndVol(sfx->currentGain * sfx->sondVolume * wii->masterGain);
            ASND_SetVoice(sfx->voice, streamFormatForChannels(sfx->channels), sr, 0, sfx->pcmData, sfx->pcmBytes, vol, vol, NULL);
            return;
        }
        WiiMusicStream* mus = findMusicById(wii, soundOrInstance);
        if (mus) mus->pitch = pitch; // pitch change on streams is approximate; would need restart
    }
}

static float wiiGetSoundPitch(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (soundOrInstance >= WII_SOUND_INSTANCE_ID_BASE) {
        WiiSoundInstance* sfx = findSfxById(wii, soundOrInstance);
        if (sfx) return sfx->pitch;
        WiiMusicStream* mus = findMusicById(wii, soundOrInstance);
        if (mus) return mus->pitch;
    }
    return 1.0f;
}

static float wiiGetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance) {
    return 0.0f; // TODO: ASND doesn't expose position easily
}

static void wiiSetTrackPosition(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t soundOrInstance, MAYBE_UNUSED float positionSeconds) {
    // TODO
}

static float wiiGetSoundLength(AudioSystem* audio, int32_t soundOrInstance) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    if (soundOrInstance >= WII_AUDIO_STREAM_INDEX_BASE) {
        int32_t streamIndex = soundOrInstance - WII_AUDIO_STREAM_INDEX_BASE;
        if (streamIndex < wii->musEntryCount) {
            WiiMusEntry* mus = &wii->musEntries[streamIndex];
            if (mus->sampleRate == 0) return 0.0f;
            return (float) mus->sampleCount / (float) mus->sampleRate;
        }
        int32_t externalIndex = streamIndex - wii->musEntryCount;
        if (externalIndex >= 0 && externalIndex < WII_MAX_EXTERNAL_STREAMS &&
            wii->externalStreams[externalIndex].active) {
            return 0.0f;
        }
    } else if (soundOrInstance < wii->sondEntryCount) {
        uint16_t audoIdx = wii->sondEntries[soundOrInstance].audoIndex;
        if (audoIdx != 0xFFFF && audoIdx < wii->audoEntryCount) {
            WiiAudoEntry* audo = &wii->audoEntries[audoIdx];
            if (audo->sampleRate == 0) return 0.0f;
            return (float) audo->sampleCount / (float) audo->sampleRate;
        }
    }
    return 0.0f;
}

static void wiiSetMasterGain(AudioSystem* audio, float gain) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    wii->masterGain = gain;
    // Update all active voices
    for (int i = 0; i < WII_MAX_SOUND_INSTANCES; i++) {
        if (wii->instances[i].active) {
            int vol = gainToAsndVol(wii->instances[i].currentGain * wii->instances[i].sondVolume * gain);
            ASND_ChangeVolumeVoice(wii->instances[i].voice, vol, vol);
        }
    }
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        if (wii->musicStreams[i].active) {
            int vol = gainToAsndVol(wii->musicStreams[i].currentGain * wii->musicStreams[i].sondVolume * gain);
            ASND_ChangeVolumeVoice(wii->musicStreams[i].voice, vol, vol);
        }
    }
}

static void wiiSetChannelCount(MAYBE_UNUSED AudioSystem* audio, MAYBE_UNUSED int32_t count) {}

static void wiiGroupLoad(AudioSystem* audio, int32_t groupIndex) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    (void) wiiEnsureAudioGroupLoaded(wii, groupIndex);
}

static bool wiiGroupIsLoaded(AudioSystem* audio, int32_t groupIndex) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    return groupIndex >= 0 &&
        groupIndex < arrlen(wii->base.audioGroups) &&
        wii->base.audioGroups[groupIndex] != NULL;
}

static int32_t wiiCreateStream(AudioSystem* audio, const char* filename) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    for (int i = 0; i < wii->musEntryCount; i++) {
        if (strcmp(wii->musEntries[i].name, filename) == 0)
            return WII_AUDIO_STREAM_INDEX_BASE + i;
    }

    if (!wii->fileSystem) {
        fprintf(stderr, "WiiAudioSystem: stream '%s' not found and no file system is available\n", filename);
        return -1;
    }

    char* resolved = wii->fileSystem->vtable->resolvePath(wii->fileSystem, filename);
    if (!resolved) {
        fprintf(stderr, "WiiAudioSystem: stream '%s' not found\n", filename);
        return -1;
    }

    FILE* f = fopen(resolved, "rb");
    if (!f) {
        fprintf(stderr, "WiiAudioSystem: stream '%s' not found (looked for %s)\n", filename, resolved);
        free(resolved);
        return -1;
    }
    fclose(f);

    int freeSlot = -1;
    for (int i = 0; i < WII_MAX_EXTERNAL_STREAMS; i++) {
        if (!wii->externalStreams[i].active) {
            freeSlot = i;
            break;
        }
    }
    if (freeSlot < 0) {
        fprintf(stderr, "WiiAudioSystem: no free external stream slots for '%s'\n", filename);
        free(resolved);
        return -1;
    }

    WiiExternalStreamEntry* ext = &wii->externalStreams[freeSlot];
    ext->active = true;
    ext->name = safeStrdup(filename);
    ext->filePath = resolved;

    int32_t streamIndex = WII_AUDIO_STREAM_INDEX_BASE + wii->musEntryCount + freeSlot;
    fprintf(stderr, "WiiAudioSystem: Created OGG stream %d for '%s' -> '%s'\n",
        streamIndex, filename, resolved);
    return streamIndex;
}

static bool wiiDestroyStream(AudioSystem* audio, int32_t streamIndex) {
    WiiAudioSystem* wii = (WiiAudioSystem*) audio;
    for (int i = 0; i < WII_MAX_MUSIC_STREAMS; i++) {
        if (wii->musicStreams[i].active && wii->musicStreams[i].soundIndex == streamIndex) {
            ASND_StopVoice(wii->musicStreams[i].voice);
            if (wii->musicStreams[i].vorbis) {
                stb_vorbis_close((stb_vorbis*) wii->musicStreams[i].vorbis);
                wii->musicStreams[i].vorbis = NULL;
            }
            wii->musicStreams[i].active = false;
        }
    }

    int32_t externalIndex = streamIndex - WII_AUDIO_STREAM_INDEX_BASE - wii->musEntryCount;
    if (externalIndex >= 0 && externalIndex < WII_MAX_EXTERNAL_STREAMS) {
        WiiExternalStreamEntry* ext = &wii->externalStreams[externalIndex];
        free(ext->name);
        free(ext->filePath);
        memset(ext, 0, sizeof(*ext));
    }
    return true;
}

// ===[ Vtable ]===

static AudioSystemVtable wiiAudioSystemVtable = {
    .init            = wiiInit,
    .destroy         = wiiDestroy,
    .update          = wiiUpdate,
    .playSound       = wiiPlaySound,
    .stopSound       = wiiStopSound,
    .stopAll         = wiiStopAll,
    .isPlaying       = wiiIsPlaying,
    .pauseSound      = wiiPauseSound,
    .resumeSound     = wiiResumeSound,
    .pauseAll        = wiiPauseAll,
    .resumeAll       = wiiResumeAll,
    .setSoundGain    = wiiSetSoundGain,
    .getSoundGain    = wiiGetSoundGain,
    .setSoundPitch   = wiiSetSoundPitch,
    .getSoundPitch   = wiiGetSoundPitch,
    .getTrackPosition = wiiGetTrackPosition,
    .setTrackPosition = wiiSetTrackPosition,
    .getSoundLength  = wiiGetSoundLength,
    .setMasterGain   = wiiSetMasterGain,
    .setChannelCount = wiiSetChannelCount,
    .groupLoad       = wiiGroupLoad,
    .groupIsLoaded   = wiiGroupIsLoaded,
    .createStream    = wiiCreateStream,
    .destroyStream   = wiiDestroyStream,
};

// ===[ Lifecycle ]===

WiiAudioSystem* WiiAudioSystem_create(void) {
    WiiAudioSystem* wii = safeCalloc(1, sizeof(WiiAudioSystem));
    wii->base.vtable = &wiiAudioSystemVtable;
    wii->masterGain = 1.0f;
    return wii;
}

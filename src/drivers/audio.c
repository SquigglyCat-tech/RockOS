#include "audio.h"

#include "io.h"
#include "pci.h"
#include "pmm.h"

#include <limits.h>

#ifndef AUDIO_BOOT_TEST_TONE
#define AUDIO_BOOT_TEST_TONE 1
#endif

#define AUDIO_PCI_VENDOR 0x8086
#define AUDIO_PCI_DEVICE 0x2415
#define AUDIO_SAMPLE_RATE 48000U
#define AUDIO_CHANNELS 2U
#define AUDIO_BYTES_PER_FRAME (AUDIO_CHANNELS * sizeof(int16_t))
#define AUDIO_MAX_DESCRIPTORS 32U
#define AUDIO_MAX_FRAMES (AUDIO_MAX_DESCRIPTORS * 32767U)
#define AUDIO_DESCRIPTOR_FRAMES 32767U
#define AUDIO_BDL_ALIGNMENT 8
#define AUDIO_BM_PCM_OUT 0x10U
#define AUDIO_BM_GLOBAL_STATUS 0x30U
#define AUDIO_BM_CODEC_READY (1U << 8)
#define AUDIO_BM_STATUS 0x06U
#define AUDIO_BM_CONTROL 0x0BU
#define AUDIO_BM_BDBAR 0x10U
#define AUDIO_BM_LVI 0x15U
#define AUDIO_BM_PICB 0x08U /* PCM-out position-in-current-buffer register */
#define AUDIO_BM_START 0x01U
#define AUDIO_BM_RESET 0x02U
#define AUDIO_BM_HALTED 0x01U
#define AUDIO_BM_CLEAR_STATUS 0x1CU
#define AUDIO_CODEC_MASTER_VOLUME 0x02U
#define AUDIO_CODEC_PCM_VOLUME 0x18U
#define AUDIO_CODEC_VOLUME_MAX 0x0000U
#define AUDIO_MAX_RESET_POLLS 100000U
#define AUDIO_TEST_TONE_FRAMES 48000U

extern const int16_t error_sound_start[];
extern const int16_t error_sound_end[];

typedef struct {
    uint32_t address;
    uint32_t control_length;
} audio_descriptor_t;

static audio_descriptor_t audio_bdl[AUDIO_MAX_DESCRIPTORS]
    __attribute__((aligned(AUDIO_BDL_ALIGNMENT)));
static int16_t audio_test_tone[AUDIO_TEST_TONE_FRAMES * AUDIO_CHANNELS];
static const int16_t audio_test_waveform[32] = {
    0, 6393, 12539, 18204, 23170, 27245, 30273, 32137,
    32767, 32137, 30273, 27245, 23170, 18204, 12539, 6393,
    0, -6393, -12539, -18204, -23170, -27245, -30273, -32137,
    -32767, -32137, -30273, -27245, -23170, -18204, -12539, -6393
};
static uint16_t codec_base;
static uint16_t bus_master_base;
static void* dma_buffer;
static size_t dma_page_count;
static bool audio_ready;
static bool audio_playing;
static bool audio_reset_completed;
static bool audio_last_play_succeeded;
static bool audio_playback_prepared;
static size_t audio_descriptor_count;
static uint16_t audio_picb_before_start;
static uint16_t audio_picb_after_start;
static const char* audio_init_status = "not initialized";

static void audio_fill_test_tone(void) {
    uint64_t phase = 0;
    uint64_t phase_step = ((uint64_t)880 << 32) / AUDIO_SAMPLE_RATE;

    for (size_t frame = 0; frame < AUDIO_TEST_TONE_FRAMES; frame++) {
        int16_t sample = audio_test_waveform[(phase >> 27) & 31];
        audio_test_tone[frame * AUDIO_CHANNELS] = sample / 2;
        audio_test_tone[frame * AUDIO_CHANNELS + 1] = sample / 2;
        phase += phase_step;
    }
}

static void audio_release_dma(void) {
    if (dma_buffer) {
        pmm_free(dma_buffer, dma_page_count);
        dma_buffer = NULL;
        dma_page_count = 0;
    }
}

bool audio_init(void) {
    audio_ready = false;
    audio_reset_completed = false;
    audio_last_play_succeeded = false;
    audio_init_status = "AC'97 controller not found";

    const pci_device_t* device = pci_find_by_id(AUDIO_PCI_VENDOR, AUDIO_PCI_DEVICE);
    if (!device) {
        return false;
    }
    audio_init_status = "invalid AC'97 I/O BAR configuration";
    if (!device->bars[0].present || !device->bars[0].is_io ||
        !device->bars[1].present || !device->bars[1].is_io ||
        device->bars[0].addr > UINT16_MAX - AUDIO_CODEC_PCM_VOLUME - 2 ||
        device->bars[1].addr > UINT16_MAX - AUDIO_BM_GLOBAL_STATUS - 4) {
        return false;
    }

    codec_base = (uint16_t)device->bars[0].addr;
    bus_master_base = (uint16_t)device->bars[1].addr;

    uint16_t command = pci_config_read16(device->bus, device->device,
        device->function, PCI_OFF_COMMAND);
    pci_config_write16(device->bus, device->device, device->function,
        PCI_OFF_COMMAND, command | 0x0005U);

    audio_init_status = "AC'97 codec did not become ready";
    bool codec_ready = false;
    for (uint32_t poll = 0; poll < AUDIO_MAX_RESET_POLLS; poll++) {
        if ((inl((uint16_t)(bus_master_base + AUDIO_BM_GLOBAL_STATUS)) &
            AUDIO_BM_CODEC_READY) != 0) {
            codec_ready = true;
            break;
        }
    }
    if (!codec_ready) {
        return false;
    }

    audio_init_status = "AC'97 PCM output reset timed out";
    outb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_CONTROL),
        AUDIO_BM_RESET);
    for (uint32_t poll = 0; poll < AUDIO_MAX_RESET_POLLS; poll++) {
        if ((inb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT +
            AUDIO_BM_CONTROL)) & AUDIO_BM_RESET) == 0) {
            audio_reset_completed = true;
            break;
        }
    }
    if (!audio_reset_completed) {
        return false;
    }

    outw((uint16_t)(codec_base + AUDIO_CODEC_MASTER_VOLUME),
        AUDIO_CODEC_VOLUME_MAX);
    outw((uint16_t)(codec_base + AUDIO_CODEC_PCM_VOLUME),
        AUDIO_CODEC_VOLUME_MAX);
    outb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_STATUS),
        AUDIO_BM_CLEAR_STATUS);
    audio_ready = true;
    audio_init_status = "ready";
    return true;
}

const char* audio_initialization_status(void) {
    return audio_init_status;
}

static bool audio_prepare_pcm(const int16_t* interleaved_stereo,
    size_t frame_count, uint32_t sample_rate) {
    if (!audio_ready || !interleaved_stereo || frame_count == 0 ||
        sample_rate != AUDIO_SAMPLE_RATE || frame_count > AUDIO_MAX_FRAMES) {
        return false;
    }

    audio_last_play_succeeded = false;
    stop();
    audio_playback_prepared = false;
    audio_descriptor_count = 0;

    size_t byte_count = frame_count * AUDIO_BYTES_PER_FRAME;
    size_t pages = (byte_count + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void* buffer = pmm_alloc(pages);
    if (!buffer) {
        return false;
    }

    int16_t* output = (int16_t*)buffer;
    size_t sample_count = frame_count * AUDIO_CHANNELS;
    for (size_t sample = 0; sample < sample_count; sample++) {
        output[sample] = interleaved_stereo[sample];
    }

    size_t frames_remaining = frame_count;
    size_t frame_offset = 0;
    while (frames_remaining != 0) {
        size_t descriptor_frames = frames_remaining > AUDIO_DESCRIPTOR_FRAMES
            ? AUDIO_DESCRIPTOR_FRAMES : frames_remaining;
        size_t descriptor_bytes = descriptor_frames * AUDIO_BYTES_PER_FRAME;
        uintptr_t address = (uintptr_t)buffer + frame_offset * AUDIO_BYTES_PER_FRAME;
        audio_bdl[audio_descriptor_count].address = (uint32_t)address;
        audio_bdl[audio_descriptor_count].control_length =
            (uint32_t)(descriptor_bytes / sizeof(int16_t)) | 0x80000000U;
        audio_descriptor_count++;
        frame_offset += descriptor_frames;
        frames_remaining -= descriptor_frames;
    }

    dma_buffer = buffer;
    dma_page_count = pages;
    outb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_CONTROL), 0);
    outb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_STATUS),
        AUDIO_BM_CLEAR_STATUS);
    outl((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_BDBAR),
        (uint32_t)(uintptr_t)audio_bdl);
    outb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_LVI),
        (uint8_t)(audio_descriptor_count - 1));
    audio_picb_before_start = inw((uint16_t)(bus_master_base +
        AUDIO_BM_PCM_OUT + AUDIO_BM_PICB));
    audio_picb_after_start = 0;
    audio_playback_prepared = true;
    return true;
}

bool audio_start_prepared(void) {
    if (!audio_ready || !audio_playback_prepared || audio_descriptor_count == 0) {
        return false;
    }

    audio_playing = true;
    outb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_CONTROL),
        AUDIO_BM_START);
    audio_picb_after_start = inw((uint16_t)(bus_master_base +
        AUDIO_BM_PCM_OUT + AUDIO_BM_PICB));
    audio_last_play_succeeded = true;
    return true;
}

bool play_pcm(const int16_t* interleaved_stereo, size_t frame_count,
    uint32_t sample_rate) {
    return audio_prepare_pcm(interleaved_stereo, frame_count, sample_rate) &&
        audio_start_prepared();
}

void audio_get_diagnostics(audio_diagnostics_t* diagnostics) {
    if (!diagnostics) {
        return;
    }

    diagnostics->pci_device_found = false;
    diagnostics->initialized = audio_ready;
    diagnostics->reset_completed = audio_reset_completed;
    diagnostics->playing = audio_playing;
    diagnostics->last_play_succeeded = audio_last_play_succeeded;
    diagnostics->playback_prepared = audio_playback_prepared;
    diagnostics->io_bars_valid = false;
    diagnostics->codec_bar_address = 0;
    diagnostics->bus_master_bar_address = 0;
    diagnostics->pci_command = 0;
    diagnostics->codec_base = codec_base;
    diagnostics->bus_master_base = bus_master_base;
    diagnostics->global_status = 0;
    diagnostics->bdl_register = 0;
    diagnostics->bdl_memory_address = (uintptr_t)audio_bdl;
    diagnostics->dma_buffer_address = (uintptr_t)dma_buffer;
    diagnostics->picb_before_start = audio_picb_before_start;
    diagnostics->picb_after_start = audio_picb_after_start;
    diagnostics->picb_current = 0;
    diagnostics->stream_status = 0;
    diagnostics->stream_control = 0;
    diagnostics->last_valid_index = 0;
    diagnostics->descriptor_count = audio_descriptor_count;

    const pci_device_t* device = pci_find_by_id(AUDIO_PCI_VENDOR,
        AUDIO_PCI_DEVICE);
    if (!device) {
        return;
    }

    diagnostics->pci_device_found = true;
    diagnostics->codec_bar_address = device->bars[0].addr;
    diagnostics->bus_master_bar_address = device->bars[1].addr;
    diagnostics->pci_command = pci_config_read16(device->bus, device->device,
        device->function, PCI_OFF_COMMAND);

    if (!device->bars[0].present || !device->bars[0].is_io ||
        !device->bars[1].present || !device->bars[1].is_io ||
        device->bars[0].addr > UINT16_MAX - AUDIO_CODEC_PCM_VOLUME - 2 ||
        device->bars[1].addr > UINT16_MAX - AUDIO_BM_GLOBAL_STATUS - 4) {
        return;
    }

    diagnostics->io_bars_valid = true;
    uint16_t bm_base = (uint16_t)device->bars[1].addr;
    uint16_t pcm_base = (uint16_t)(bm_base + AUDIO_BM_PCM_OUT);
    diagnostics->global_status = inl((uint16_t)(bm_base +
        AUDIO_BM_GLOBAL_STATUS));
    diagnostics->stream_status = inb((uint16_t)(pcm_base + AUDIO_BM_STATUS));
    diagnostics->picb_current = inw((uint16_t)(pcm_base + AUDIO_BM_PICB));
    diagnostics->stream_control = inb((uint16_t)(pcm_base +
        AUDIO_BM_CONTROL));
    diagnostics->bdl_register = inl((uint16_t)(pcm_base + AUDIO_BM_BDBAR));
    diagnostics->last_valid_index = inb((uint16_t)(pcm_base + AUDIO_BM_LVI));
}

bool audio_get_descriptor(size_t index, audio_descriptor_info_t* descriptor) {
    if (!descriptor || index >= audio_descriptor_count) {
        return false;
    }

    descriptor->buffer_address = audio_bdl[index].address;
    descriptor->sample_count =
        (uint16_t)(audio_bdl[index].control_length & 0xFFFFU);
    descriptor->control_status =
        (uint16_t)(audio_bdl[index].control_length >> 16);
    return true;
}

void stop(void) {
    if (!audio_playing) {
        if (audio_playback_prepared) {
            audio_playback_prepared = false;
            audio_descriptor_count = 0;
            audio_release_dma();
        }
        return;
    }

    outb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_CONTROL), 0);
    for (uint32_t poll = 0; poll < AUDIO_MAX_RESET_POLLS; poll++) {
        if (inb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT +
            AUDIO_BM_STATUS)) & AUDIO_BM_HALTED) {
            break;
        }
    }
    outb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_CONTROL),
        AUDIO_BM_RESET);
    outb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_STATUS),
        AUDIO_BM_CLEAR_STATUS);
    audio_playing = false;
    audio_playback_prepared = false;
    audio_descriptor_count = 0;
    audio_release_dma();
}

void audio_poll(void) {
    if (audio_playing &&
        (inb((uint16_t)(bus_master_base + AUDIO_BM_PCM_OUT + AUDIO_BM_STATUS)) &
         AUDIO_BM_HALTED)) {
        stop();
    }
}

void audio_play_test_tone(void) {
#if AUDIO_BOOT_TEST_TONE
    if (!audio_ready) {
        return;
    }
    audio_fill_test_tone();
    play_pcm(audio_test_tone, AUDIO_TEST_TONE_FRAMES, AUDIO_SAMPLE_RATE);
#endif
}

bool audio_prepare_test_tone(void) {
    audio_fill_test_tone();
    return audio_prepare_pcm(audio_test_tone, AUDIO_TEST_TONE_FRAMES,
        AUDIO_SAMPLE_RATE);
}

void audio_play_error_sound(void) {
    size_t sample_count = ((uintptr_t)error_sound_end -
        (uintptr_t)error_sound_start) / sizeof(*error_sound_start);
    play_pcm(error_sound_start, sample_count / AUDIO_CHANNELS,
        AUDIO_SAMPLE_RATE);
}
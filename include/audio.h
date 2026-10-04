#ifndef ROCKOS_AUDIO_H
#define ROCKOS_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool audio_init(void);
const char* audio_initialization_status(void);
bool play_pcm(const int16_t* interleaved_stereo, size_t frame_count,
    uint32_t sample_rate);
typedef struct {
    bool pci_device_found;
    bool initialized;
    bool reset_completed;
    bool playing;
    bool last_play_succeeded;
    bool playback_prepared;
    bool io_bars_valid;
    uint64_t codec_bar_address;
    uint64_t bus_master_bar_address;
    uint16_t pci_command;
    uint16_t codec_base;
    uint16_t bus_master_base;
    uint32_t global_status;
    uint32_t bdl_register;
    uintptr_t bdl_memory_address;
    uintptr_t dma_buffer_address;
    uint16_t picb_before_start;
    uint16_t picb_after_start;
    uint16_t picb_current;
    uint8_t stream_status;
    uint8_t stream_control;
    uint8_t last_valid_index;
    size_t descriptor_count;
} audio_diagnostics_t;

typedef struct {
    uintptr_t buffer_address;
    uint16_t sample_count;
    uint16_t control_status;
} audio_descriptor_info_t;

void audio_get_diagnostics(audio_diagnostics_t* diagnostics);
bool audio_get_descriptor(size_t index, audio_descriptor_info_t* descriptor);
bool audio_prepare_test_tone(void);
bool audio_start_prepared(void);
void stop(void);
void audio_poll(void);
void audio_play_test_tone(void);
void audio_play_error_sound(void);

#endif
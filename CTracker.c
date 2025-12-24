// CTracker.c
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/select.h>
#include <ctype.h>
#include <stdint.h>

#define SAMPLE_RATE 44100
#define MAX_ROWS 16
#define MAX_CHANNELS 8
#define MAX_POLYPHONY 8  // Maximum number of simultaneous notes

// Note names
const char* NOTE_NAMES[] = {
    "C-2", "C#-2", "D-2", "D#-2", "E-2", "F-2", "F#-2", "G-2", "G#-2", "A-2", "A#-2", "B-2",
    "C-1", "C#-1", "D-1", "D#-1", "E-1", "F-1", "F#-1", "G-1", "G#-1", "A-1", "A#-1", "B-1",
    "C0", "C#0", "D0", "D#0", "E0", "F0", "F#0", "G0", "G#0", "A0", "A#0", "B0",
    "C1", "C#1", "D1", "D#1", "E1", "F1", "F#1", "G1", "G#1", "A1", "A#1", "B1",
    "C2", "C#2", "D2", "D#2", "E2", "F2", "F#2", "G2", "G#2", "A2", "A#2", "B2",
    "C3", "C#3", "D3", "D#3", "E3", "F3", "F#3", "G3", "G#3", "A3", "A#3", "B3",
    "C4", "C#4", "D4", "D#4", "E4", "F4", "F#4", "G4", "G#4", "A4", "A#4", "B4",
    "C5", "C#5", "D5", "D#5", "E5", "F5", "F#5", "G5", "G#5", "A5", "A#5", "B5",
    "C6", "C#6", "D6", "D#6", "E6", "F6", "F#6", "G6", "G#6", "A6", "A#6", "B6",
    "C7", "C#7", "D7", "D#7", "E7", "F7", "F#7", "G7", "G#7", "A7", "A#7", "B7",
    "C8", "C#8", "D8", "D#8", "E8", "F8", "F#8", "G8", "G#8", "A8", "A#8", "B8",
    "C9", "C#9", "D9", "D#9", "E9", "F9", "F#9", "G9"
};

#define TOTAL_NOTES 128
const char* REST_NAME = "---";

typedef struct {
    int note;           // MIDI note (0 = rest)
    int original_note;  // Original note of the sample
    int duration_ms;    // duration
    char sample[64];    // WAV file
    int playing;        // Playback flag
    double pitch_ratio; // Pitch shift ratio for sample
} Cell;

typedef struct {
    Cell cells[MAX_ROWS];
} Track;

typedef struct {
    Track channels[MAX_CHANNELS];
    int num_channels;
    int num_rows;
    int bpm;           // Added BPM
    int loop_start;    // Loop start row
    int loop_end;      // Loop end row
    int loop_enabled;  // Loop enabled flag
} Song;

typedef struct {
    double freq;
    int duration_ms;
    int active;
    pthread_t thread;
} ToneThreadData;

typedef struct {
    char filename[64];
    int note;
    int target_note;
    int channel;
    int duration_ms;
    double pitch_ratio;
    int active;
    pthread_t thread;
} SampleThreadData;

// Global data for tones and samples
ToneThreadData tone_threads[MAX_CHANNELS];
SampleThreadData sample_threads[MAX_CHANNELS];
pthread_mutex_t audio_mutex = PTHREAD_MUTEX_INITIALIZER;
int global_playing = 0; // Flag for stopping playback

// ---- WAV file header structure ----
#pragma pack(push, 1)
typedef struct {
    char     riff[4];        // "RIFF"
    uint32_t file_size;      // File size - 8
    char     wave[4];        // "WAVE"
    char     fmt[4];         // "fmt "
    uint32_t fmt_size;       // Format size (16 for PCM)
    uint16_t audio_format;   // Audio format (1 for PCM)
    uint16_t num_channels;   // Number of channels
    uint32_t sample_rate;    // Sample rate
    uint32_t byte_rate;      // Byte rate = sample_rate * num_channels * bits_per_sample/8
    uint16_t block_align;    // Block align = num_channels * bits_per_sample/8
    uint16_t bits_per_sample;// Bits per sample
    char     data[4];        // "data"
    uint32_t data_size;      // Data size
} WavHeader;
#pragma pack(pop)

// ---- Function to calculate note duration based on BPM ----
int get_note_duration_ms(Song* song) {
    // Duration of one row in milliseconds
    // Formula: (60000 / BPM) / 4 (assumes 4 rows per beat)
    if (song->bpm == 0) return 500; // Protection from division by zero
    return (60000 / song->bpm) / 4;
}

// ---- Function to pitch shift a sample ----
Sint16* pitch_shift_sample(const Sint16* original, Uint32 original_len, 
                          double pitch_ratio, Uint32* new_len) {
    if (fabs(pitch_ratio - 1.0) < 0.001) {
        // No pitch shift needed
        Sint16* copy = malloc(original_len * sizeof(Sint16));
        if (copy) {
            memcpy(copy, original, original_len * sizeof(Sint16));
            *new_len = original_len;
        }
        return copy;
    }
    
    if (pitch_ratio > 2.0) pitch_ratio = 2.0;  // Limit pitch shift
    if (pitch_ratio < 0.5) pitch_ratio = 0.5;
    
    *new_len = (Uint32)(original_len / pitch_ratio);
    Sint16* shifted = malloc(*new_len * sizeof(Sint16));
    
    if (!shifted) return NULL;
    
    // Simple linear interpolation for pitch shifting
    for (Uint32 i = 0; i < *new_len; i++) {
        double src_pos = i * pitch_ratio;
        Uint32 idx1 = (Uint32)src_pos;
        Uint32 idx2 = idx1 + 1;
        
        if (idx2 >= original_len) idx2 = original_len - 1;
        
        double frac = src_pos - idx1;
        double sample = original[idx1] * (1.0 - frac) + original[idx2] * frac;
        
        shifted[i] = (Sint16)sample;
    }
    
    return shifted;
}

// ---- Thread function for playing pitch-shifted samples ----
void* play_sample_thread(void* arg) {
    SampleThreadData* data = (SampleThreadData*)arg;
    
    if (strlen(data->filename) == 0 || !data->active) {
        return NULL;
    }
    
    Mix_Chunk* sound = Mix_LoadWAV(data->filename);
    if (!sound) {
        printf("Failed to load WAV: %s\n", Mix_GetError());
        return NULL;
    }
    
    if (fabs(data->pitch_ratio - 1.0) > 0.001) {
        // Apply pitch shift
        Uint32 original_len = sound->alen / sizeof(Sint16);
        Uint32 new_len;
        
        Sint16* shifted_data = pitch_shift_sample((Sint16*)sound->abuf, 
                                                 original_len, 
                                                 data->pitch_ratio, 
                                                 &new_len);
        
        if (shifted_data) {
            // Create a new chunk with pitch-shifted data
            Mix_FreeChunk(sound);
            sound = Mix_QuickLoad_RAW((Uint8*)shifted_data, new_len * sizeof(Sint16));
            if (sound) {
                sound->allocated = 1; // Mark as allocated so SDL_mixer will free it
            }
        }
    }
    
    if (!sound) {
        return NULL;
    }
    
    // Calculate playback speed based on pitch ratio
    // Lower pitch = longer duration, higher pitch = shorter duration
    int play_duration = (int)(data->duration_ms / data->pitch_ratio);
    
    // Play on specific channel
    Mix_PlayChannel(data->channel, sound, 0);
    
    // Wait for playback to complete (approximate)
    SDL_Delay(play_duration);
    
    pthread_mutex_lock(&audio_mutex);
    data->active = 0;
    pthread_mutex_unlock(&audio_mutex);
    
    return NULL;
}

// ---- Function to play a tone in a separate thread ----
void* play_tone_thread(void* arg) {
    ToneThreadData* data = (ToneThreadData*)arg;
    
    if (data->freq == 0 || !data->active) {
        return NULL;
    }
    
    Uint32 samples = data->duration_ms * SAMPLE_RATE / 1000;
    Sint16* buffer = malloc(sizeof(Sint16) * samples);
    if (!buffer) return NULL;
    
    // Generate sine wave
    for (Uint32 i = 0; i < samples; i++) {
        buffer[i] = (Sint16)(32767 * sin(2.0 * M_PI * data->freq * i / SAMPLE_RATE));
    }
    
    SDL_AudioSpec spec;
    SDL_zero(spec);
    spec.freq = SAMPLE_RATE;
    spec.format = AUDIO_S16SYS;
    spec.channels = 1;
    spec.samples = 4096;
    spec.callback = NULL;
    
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &spec, NULL, 0);
    if (dev == 0) {
        free(buffer);
        return NULL;
    }
    
    SDL_QueueAudio(dev, buffer, samples * sizeof(Sint16));
    SDL_PauseAudioDevice(dev, 0);
    
    // Wait for playback to complete
    SDL_Delay(data->duration_ms);
    
    SDL_CloseAudioDevice(dev);
    free(buffer);
    
    pthread_mutex_lock(&audio_mutex);
    data->active = 0;
    pthread_mutex_unlock(&audio_mutex);
    
    return NULL;
}

// ---- Convert note name to MIDI number ----
int note_name_to_midi(const char* note_name) {
    if (strcmp(note_name, "---") == 0 || strcmp(note_name, "") == 0) {
        return 0; // Rest
    }
    
    for (int i = 0; i < TOTAL_NOTES; i++) {
        if (strcmp(note_name, NOTE_NAMES[i]) == 0) {
            return i;
        }
    }
    
    // Try case-insensitive comparison
    for (int i = 0; i < TOTAL_NOTES; i++) {
        char upper_note[16];
        char upper_input[16];
        
        strcpy(upper_note, NOTE_NAMES[i]);
        strcpy(upper_input, note_name);
        
        // Convert to uppercase for comparison
        for (int j = 0; upper_note[j]; j++) upper_note[j] = toupper(upper_note[j]);
        for (int j = 0; upper_input[j]; j++) upper_input[j] = toupper(upper_input[j]);
        
        if (strcmp(upper_note, upper_input) == 0) {
            return i;
        }
    }
    
    return 0; // Default to rest if not found
}

// ---- Convert MIDI number to note name ----
const char* midi_to_note_name(int midi_note) {
    if (midi_note <= 0 || midi_note >= TOTAL_NOTES) {
        return REST_NAME;
    }
    return NOTE_NAMES[midi_note];
}

// ---- Calculate pitch shift ratio ----
double calculate_pitch_ratio(int original_note, int target_note) {
    if (original_note <= 0 || target_note <= 0) return 1.0;
    
    // Calculate semitone difference
    int semitones = target_note - original_note;
    
    // Pitch ratio = 2^(semitones/12)
    return pow(2.0, semitones / 12.0);
}

// ---- TTY display ----
void draw_tty(Song* song, int cursor_row, int cursor_channel) {
    system("clear");
    printf("CTracker (TTY) | BPM: %d", song->bpm);
    
    // Display loop status
    if (song->loop_enabled) {
        printf(" | LOOP: %d-%d", song->loop_start, song->loop_end);
    } else {
        printf(" | LOOP: OFF");
    }
    printf("\n\n");
    
    // Channel number headers
    printf("    ");
    for (int ch = 0; ch < song->num_channels; ch++) {
        printf("Ch%02d ", ch);
    }
    printf("\n");
    
    // Separator line
    printf("   +");
    for (int ch = 0; ch < song->num_channels; ch++) {
        printf("-----");
    }
    printf("\n");
    
    // Pattern rows
    for (int r = 0; r < song->num_rows; r++) {
        // Mark loop range
        if (song->loop_enabled && r == song->loop_start) printf("[");
        else if (song->loop_enabled && r == song->loop_end) printf("]");
        else printf(" ");
        
        printf("%02d |", r);
        for (int ch = 0; ch < song->num_channels; ch++) {
            if (r == cursor_row && ch == cursor_channel) printf(">");
            else printf(" ");
            
            Cell* c = &song->channels[ch].cells[r];
            if (c->note > 0) {
                const char* note_name = midi_to_note_name(c->note);
                printf("%-4s", note_name);
            } else {
                printf("%-4s", REST_NAME);
            }
        }
        printf("\n");
    }
}

// ---- Edit cell ----
void edit_cell(Song* song, int row, int channel) {
    char input[64];
    char sample[64];
    int original_note = 60; // Default C4 for new samples
    
    printf("Current note: ");
    if (song->channels[channel].cells[row].note > 0) {
        printf("%s", midi_to_note_name(song->channels[channel].cells[row].note));
    } else {
        printf("--- (rest)");
    }
    printf("\n");
    
    // Check if there's already a sample
    if (strlen(song->channels[channel].cells[row].sample) > 0) {
        printf("Current sample: %s (base note: %s)\n", 
               song->channels[channel].cells[row].sample,
               midi_to_note_name(song->channels[channel].cells[row].original_note));
        original_note = song->channels[channel].cells[row].original_note;
    }
    
    printf("Enter note (e.g., C4, A#3, F-1) or '---' for rest: ");
    fgets(input, sizeof(input), stdin);
    input[strcspn(input, "\n")] = 0; // remove \n
    
    printf("Enter WAV file (leave empty if none): ");
    fgets(sample, sizeof(sample), stdin);
    sample[strcspn(sample, "\n")] = 0; // remove \n
    
    int note = note_name_to_midi(input);
    
    // If a new sample is provided, ask for its original note
    if (strlen(sample) > 0 && note > 0) {
        char orig_note_input[64];
        printf("What is the original note of this sample? (e.g., C4): ");
        fgets(orig_note_input, sizeof(orig_note_input), stdin);
        orig_note_input[strcspn(orig_note_input, "\n")] = 0;
        
        original_note = note_name_to_midi(orig_note_input);
        if (original_note <= 0) original_note = 60; // Default to C4
    } else if (strlen(sample) == 0 && strlen(song->channels[channel].cells[row].sample) > 0) {
        // Keep original note if sample stays the same
        original_note = song->channels[channel].cells[row].original_note;
    }
    
    song->channels[channel].cells[row].note = note;
    song->channels[channel].cells[row].original_note = original_note;
    song->channels[channel].cells[row].duration_ms = get_note_duration_ms(song);
    strcpy(song->channels[channel].cells[row].sample, sample);
    
    // Calculate pitch ratio
    if (note > 0 && original_note > 0) {
        song->channels[channel].cells[row].pitch_ratio = 
            calculate_pitch_ratio(original_note, note);
    } else {
        song->channels[channel].cells[row].pitch_ratio = 1.0;
    }
    
    printf("Set to: ");
    if (note > 0) {
        printf("%s", midi_to_note_name(note));
        if (strlen(sample) > 0) {
            printf(" (sample: %s, original: %s, pitch: %.3fx)", 
                   sample, 
                   midi_to_note_name(original_note),
                   song->channels[channel].cells[row].pitch_ratio);
        }
    } else {
        printf("--- (rest)");
    }
    printf("\n");
}

// ---- Non-blocking input check (simplified version) ----
int kbhit() {
    struct timeval tv = {0L, 0L};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(0, &fds);
    return select(1, &fds, NULL, NULL, &tv);
}

// ---- Play row with 8 sounds simultaneously ----
void play_row(Song* song, int row) {
    int note_duration = get_note_duration_ms(song);
    
    printf("Row %02d: ", row);
    
    // Stop all previous playback
    for (int i = 0; i < MAX_CHANNELS; i++) {
        pthread_mutex_lock(&audio_mutex);
        if (tone_threads[i].active) {
            tone_threads[i].active = 0;
        }
        if (sample_threads[i].active) {
            sample_threads[i].active = 0;
        }
        pthread_mutex_unlock(&audio_mutex);
    }
    
    // Wait a bit for threads to finish
    SDL_Delay(10);
    
    // Start playback for each channel with active note
    int active_channels = 0;
    for (int ch = 0; ch < song->num_channels; ch++) {
        Cell* c = &song->channels[ch].cells[row];
        
        if (strlen(c->sample) > 0 && c->note > 0) {
            // Play WAV file with pitch shifting
            pthread_mutex_lock(&audio_mutex);
            
            strcpy(sample_threads[ch].filename, c->sample);
            sample_threads[ch].note = c->original_note;
            sample_threads[ch].target_note = c->note;
            sample_threads[ch].channel = ch;
            sample_threads[ch].duration_ms = note_duration;
            sample_threads[ch].pitch_ratio = c->pitch_ratio;
            sample_threads[ch].active = 1;
            
            pthread_create(&sample_threads[ch].thread, NULL, play_sample_thread, &sample_threads[ch]);
            pthread_detach(sample_threads[ch].thread); // Auto-cleanup on completion
            
            pthread_mutex_unlock(&audio_mutex);
            
            const char* note_name = midi_to_note_name(c->note);
            const char* orig_name = midi_to_note_name(c->original_note);
            printf("Ch%d:%s(SMP: %s->%s %.2fx) ", 
                   ch, note_name, orig_name, note_name, c->pitch_ratio);
            active_channels++;
            
        } else if (c->note > 0) {
            // Play tone in separate thread
            pthread_mutex_lock(&audio_mutex);
            tone_threads[ch].freq = 440.0 * pow(2, (c->note - 69) / 12.0);
            tone_threads[ch].duration_ms = note_duration;
            tone_threads[ch].active = 1;
            
            pthread_create(&tone_threads[ch].thread, NULL, play_tone_thread, &tone_threads[ch]);
            pthread_detach(tone_threads[ch].thread); // Auto-cleanup on completion
            
            pthread_mutex_unlock(&audio_mutex);
            
            const char* note_name = midi_to_note_name(c->note);
            printf("Ch%d:%s ", ch, note_name);
            active_channels++;
        }
    }
    
    if (active_channels == 0) {
        printf("--- rest ---");
    }
    printf("\n");
    
    // Wait for note duration
    SDL_Delay(note_duration);
}

// ---- Play entire song with loop support ----
void play_song(Song* song) {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        printf("SDL_Init error: %s\n", SDL_GetError());
        return;
    }
    
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        printf("Mix_OpenAudio error: %s\n", Mix_GetError());
        SDL_Quit();
        return;
    }
    
    // Allocate 8 channels for SDL_mixer
    Mix_AllocateChannels(8);
    
    int note_duration = get_note_duration_ms(song);
    printf("Playing... BPM: %d, Note duration: %dms\n", song->bpm, note_duration);
    
    if (song->loop_enabled) {
        printf("Loop enabled: rows %d to %d\n", song->loop_start, song->loop_end);
    }
    printf("Press any key to stop...\n");
    
    // Initialize thread data
    for (int i = 0; i < MAX_CHANNELS; i++) {
        tone_threads[i].active = 0;
        tone_threads[i].freq = 0;
        sample_threads[i].active = 0;
        sample_threads[i].filename[0] = '\0';
    }
    
    global_playing = 1;
    
    // Save original terminal settings
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    
    // Play all rows with loop support
    int current_row = 0;
    int loop_count = 0;
    
    while (global_playing) {
        play_row(song, current_row);
        
        // Check if a key was pressed
        if (kbhit()) {
            getchar(); // Read the pressed key
            global_playing = 0;
            printf("\nPlayback stopped\n");
            break;
        }
        
        // Move to next row
        current_row++;
        
        // Handle loop
        if (song->loop_enabled) {
            if (current_row > song->loop_end) {
                current_row = song->loop_start;
                loop_count++;
                printf("Loop %d\n", loop_count);
            }
        } else {
            // No loop - stop at end of pattern
            if (current_row >= song->num_rows) {
                break;
            }
        }
    }
    
    // Restore terminal settings
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    
    // Stop all playback
    for (int i = 0; i < MAX_CHANNELS; i++) {
        pthread_mutex_lock(&audio_mutex);
        tone_threads[i].active = 0;
        sample_threads[i].active = 0;
        pthread_mutex_unlock(&audio_mutex);
    }
    
    // Wait for all threads to finish
    SDL_Delay(100);
    
    Mix_CloseAudio();
    SDL_Quit();
    printf("Playback finished. Total loops: %d\n", loop_count);
}

// ---- Change BPM ----
void change_bpm(Song* song) {
    int new_bpm;
    printf("Current BPM: %d\n", song->bpm);
    printf("Enter new BPM (20-300): ");
    scanf("%d", &new_bpm);
    getchar(); // remove newline character
    
    if (new_bpm >= 20 && new_bpm <= 300) {
        song->bpm = new_bpm;
        // Update durations of all notes
        for (int ch = 0; ch < song->num_channels; ch++) {
            for (int row = 0; row < song->num_rows; row++) {
                song->channels[ch].cells[row].duration_ms = get_note_duration_ms(song);
            }
        }
        printf("BPM changed to %d\n", song->bpm);
    } else {
        printf("Invalid BPM value\n");
    }
}

// ---- Set loop points ----
void set_loop(Song* song) {
    int start, end;
    
    printf("Current loop: ");
    if (song->loop_enabled) {
        printf("ON, rows %d-%d\n", song->loop_start, song->loop_end);
    } else {
        printf("OFF\n");
    }
    
    printf("Enable loop? (y/n): ");
    char choice = getchar();
    getchar(); // remove newline
    
    if (choice == 'y' || choice == 'Y') {
        song->loop_enabled = 1;
        
        printf("Enter loop start row (0-%d): ", song->num_rows - 1);
        scanf("%d", &start);
        getchar();
        
        printf("Enter loop end row (%d-%d): ", start + 1, song->num_rows - 1);
        scanf("%d", &end);
        getchar();
        
        // Validate loop points
        if (start >= 0 && start < end && end < song->num_rows) {
            song->loop_start = start;
            song->loop_end = end;
            printf("Loop set to rows %d-%d\n", start, end);
        } else {
            printf("Invalid loop range. Loop disabled.\n");
            song->loop_enabled = 0;
        }
    } else {
        song->loop_enabled = 0;
        printf("Loop disabled\n");
    }
}

// ---- Play current row (for testing) ----
void play_current_row(Song* song, int row) {
    printf("Playing row %d...\n", row);
    
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        printf("SDL_Init error: %s\n", SDL_GetError());
        return;
    }
    
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        printf("Mix_OpenAudio error: %s\n", Mix_GetError());
        SDL_Quit();
        return;
    }
    
    Mix_AllocateChannels(8);
    
    // Initialize thread data
    for (int i = 0; i < MAX_CHANNELS; i++) {
        tone_threads[i].active = 0;
        tone_threads[i].freq = 0;
        sample_threads[i].active = 0;
        sample_threads[i].filename[0] = '\0';
    }
    
    play_row(song, row);
    
    // Stop all playback
    for (int i = 0; i < MAX_CHANNELS; i++) {
        pthread_mutex_lock(&audio_mutex);
        tone_threads[i].active = 0;
        sample_threads[i].active = 0;
        pthread_mutex_unlock(&audio_mutex);
    }
    
    SDL_Delay(100);
    
    Mix_CloseAudio();
    SDL_Quit();
}

// ---- Get character without Enter ----
int getch() {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    int ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ch;
}

// ---- Generate sine wave for a note ----
void generate_sine_wave(Sint16* buffer, Uint32 samples, double freq, double volume) {
    for (Uint32 i = 0; i < samples; i++) {
        buffer[i] = (Sint16)(32767 * volume * sin(2.0 * M_PI * freq * i / SAMPLE_RATE));
    }
}

// ---- Save song to WAV file ----
int save_song_to_wav(Song* song, const char* filename) {
    // Calculate total duration in samples
    int note_duration = get_note_duration_ms(song);
    int total_rows = song->num_rows;
    
    if (song->loop_enabled && song->loop_end > song->loop_start) {
        // For looped songs, render a few loops
        total_rows = song->loop_end - song->loop_start + 1;
        total_rows *= 4; // Render 4 loops
    }
    
    Uint32 total_samples = total_rows * note_duration * SAMPLE_RATE / 1000;
    
    // Allocate audio buffer (stereo)
    Sint16* audio_buffer = calloc(total_samples * 2, sizeof(Sint16));
    if (!audio_buffer) {
        printf("Error: Could not allocate audio buffer\n");
        return 0;
    }
    
    printf("Rendering %d rows to WAV...\n", total_rows);
    
    // Initialize SDL_mixer for sample loading
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        printf("SDL_Init error: %s\n", SDL_GetError());
        free(audio_buffer);
        return 0;
    }
    
    if (Mix_OpenAudio(SAMPLE_RATE, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        printf("Mix_OpenAudio error: %s\n", Mix_GetError());
        free(audio_buffer);
        SDL_Quit();
        return 0;
    }
    
    Mix_AllocateChannels(8);
    
    // Render each row
    int current_row = 0;
    int loops_done = 0;
    
    while (current_row < total_rows) {
        int actual_row = current_row % song->num_rows;
        
        if (song->loop_enabled && actual_row > song->loop_end) {
            actual_row = song->loop_start;
            loops_done++;
            if (loops_done >= 4) break; // Only render 4 loops
        }
        
        printf("Rendering row %d/%d (actual: %d)\r", current_row + 1, total_rows, actual_row);
        fflush(stdout);
        
        // Calculate position in buffer
        Uint32 start_sample = current_row * note_duration * SAMPLE_RATE / 1000;
        Uint32 row_samples = note_duration * SAMPLE_RATE / 1000;
        
        // Process each channel
        for (int ch = 0; ch < song->num_channels; ch++) {
            Cell* c = &song->channels[ch].cells[actual_row];
            
            if (c->note > 0) {
                Sint16* row_buffer = calloc(row_samples * 2, sizeof(Sint16));
                if (!row_buffer) continue;
                
                if (strlen(c->sample) > 0) {
                    // Load and process sample
                    Mix_Chunk* sound = Mix_LoadWAV(c->sample);
                    if (sound) {
                        Uint32 original_len = sound->alen / sizeof(Sint16);
                        Uint32 new_len;
                        
                        Sint16* sample_data = NULL;
                        
                        // Apply pitch shifting if needed
                        if (fabs(c->pitch_ratio - 1.0) > 0.001) {
                            sample_data = pitch_shift_sample((Sint16*)sound->abuf, 
                                                           original_len, 
                                                           c->pitch_ratio, 
                                                           &new_len);
                        } else {
                            new_len = original_len;
                            sample_data = malloc(new_len * sizeof(Sint16));
                            if (sample_data) {
                                memcpy(sample_data, sound->abuf, new_len * sizeof(Sint16));
                            }
                        }
                        
                        if (sample_data) {
                            // Mix sample into row buffer
                            Uint32 samples_to_mix = new_len < row_samples ? new_len : row_samples;
                            
                            // Convert mono to stereo and mix
                            for (Uint32 i = 0; i < samples_to_mix; i++) {
                                Sint16 sample = sample_data[i];
                                // Apply panning: channel 0-3 left, 4-7 right
                                float pan = ch < 4 ? 0.7f : 0.3f;
                                
                                if (ch < 4) {
                                    // Left channel
                                    int mixed = row_buffer[i*2] + (Sint16)(sample * pan);
                                    if (mixed > 32767) mixed = 32767;
                                    if (mixed < -32768) mixed = -32768;
                                    row_buffer[i*2] = (Sint16)mixed;
                                } else {
                                    // Right channel
                                    int mixed = row_buffer[i*2+1] + (Sint16)(sample * (1.0f - pan));
                                    if (mixed > 32767) mixed = 32767;
                                    if (mixed < -32768) mixed = -32768;
                                    row_buffer[i*2+1] = (Sint16)mixed;
                                }
                            }
                            
                            free(sample_data);
                        }
                        
                        Mix_FreeChunk(sound);
                    }
                } else {
                    // Generate sine wave
                    double freq = 440.0 * pow(2, (c->note - 69) / 12.0);
                    
                    // Convert mono to stereo with panning
                    for (Uint32 i = 0; i < row_samples; i++) {
                        Sint16 sample = (Sint16)(32767 * 0.3 * sin(2.0 * M_PI * freq * i / SAMPLE_RATE));
                        
                        // Apply panning: channel 0-3 left, 4-7 right
                        float pan = ch < 4 ? 0.7f : 0.3f;
                        
                        if (ch < 4) {
                            // Left channel
                            int mixed = row_buffer[i*2] + (Sint16)(sample * pan);
                            if (mixed > 32767) mixed = 32767;
                            if (mixed < -32768) mixed = -32768;
                            row_buffer[i*2] = (Sint16)mixed;
                        } else {
                            // Right channel
                            int mixed = row_buffer[i*2+1] + (Sint16)(sample * (1.0f - pan));
                            if (mixed > 32767) mixed = 32767;
                            if (mixed < -32768) mixed = -32768;
                            row_buffer[i*2+1] = (Sint16)mixed;
                        }
                    }
                }
                
                // Mix row buffer into main buffer
                for (Uint32 i = 0; i < row_samples; i++) {
                    Uint32 dest_idx = (start_sample + i) * 2;
                    
                    // Left channel
                    int mixed_left = audio_buffer[dest_idx] + row_buffer[i*2];
                    if (mixed_left > 32767) mixed_left = 32767;
                    if (mixed_left < -32768) mixed_left = -32768;
                    audio_buffer[dest_idx] = (Sint16)mixed_left;
                    
                    // Right channel
                    int mixed_right = audio_buffer[dest_idx+1] + row_buffer[i*2+1];
                    if (mixed_right > 32767) mixed_right = 32767;
                    if (mixed_right < -32768) mixed_right = -32768;
                    audio_buffer[dest_idx+1] = (Sint16)mixed_right;
                }
                
                free(row_buffer);
            }
        }
        
        current_row++;
    }
    
    printf("\nDone rendering audio.\n");
    
    // Create WAV file
    FILE* wav_file = fopen(filename, "wb");
    if (!wav_file) {
        printf("Error: Could not create WAV file\n");
        free(audio_buffer);
        Mix_CloseAudio();
        SDL_Quit();
        return 0;
    }
    
    // Prepare WAV header
    WavHeader header;
    memcpy(header.riff, "RIFF", 4);
    memcpy(header.wave, "WAVE", 4);
    memcpy(header.fmt, "fmt ", 4);
    memcpy(header.data, "data", 4);
    
    header.fmt_size = 16;
    header.audio_format = 1; // PCM
    header.num_channels = 2; // Stereo
    header.sample_rate = SAMPLE_RATE;
    header.bits_per_sample = 16;
    header.block_align = header.num_channels * header.bits_per_sample / 8;
    header.byte_rate = header.sample_rate * header.block_align;
    
    header.data_size = total_samples * 2 * sizeof(Sint16);
    header.file_size = header.data_size + sizeof(WavHeader) - 8;
    
    // Write header
    fwrite(&header, sizeof(WavHeader), 1, wav_file);
    
    // Write audio data
    fwrite(audio_buffer, sizeof(Sint16), total_samples * 2, wav_file);
    
    fclose(wav_file);
    free(audio_buffer);
    
    Mix_CloseAudio();
    SDL_Quit();
    
    printf("Song saved to %s (%d samples, %.2f seconds)\n", 
           filename, total_samples, (float)total_samples / SAMPLE_RATE);
    
    return 1;
}

// ---- Load song from file ----
int load_song(Song* song, const char* filename) {
    FILE* file = fopen(filename, "r");
    if (!file) {
        printf("Error: Could not open file %s\n", filename);
        return 0;
    }
    
    printf("Loading song from %s...\n", filename);
    
    // Read header
    char header[64];
    fgets(header, sizeof(header), file); // CTracker Song
    
    if (fscanf(file, "BPM: %d\n", &song->bpm) != 1) {
        printf("Error reading BPM\n");
        fclose(file);
        return 0;
    }
    
    if (fscanf(file, "Rows: %d\n", &song->num_rows) != 1) {
        printf("Error reading rows\n");
        fclose(file);
        return 0;
    }
    
    if (fscanf(file, "Channels: %d\n", &song->num_channels) != 1) {
        printf("Error reading channels\n");
        fclose(file);
        return 0;
    }
    
    if (fscanf(file, "Loop: %d %d %d\n", &song->loop_enabled, &song->loop_start, &song->loop_end) != 3) {
        printf("Error reading loop settings\n");
        fclose(file);
        return 0;
    }
    
    int note_duration = get_note_duration_ms(song);
    
    // Read each cell
    for (int ch = 0; ch < song->num_channels; ch++) {
        for (int row = 0; row < song->num_rows; row++) {
            Cell* c = &song->channels[ch].cells[row];
            char note_str[16];
            char sample[64];
            
            if (fscanf(file, "%d %d %15s %63[^\n]\n", 
                   &c->note, &c->original_note, note_str, sample) != 4) {
                printf("Error reading cell at channel %d, row %d\n", ch, row);
                fclose(file);
                return 0;
            }
            
            // Clean up sample string (remove newline if present)
            sample[strcspn(sample, "\n")] = 0;
            
            strcpy(c->sample, sample);
            c->duration_ms = note_duration;
            
            if (c->note > 0 && c->original_note > 0) {
                c->pitch_ratio = calculate_pitch_ratio(c->original_note, c->note);
            } else {
                c->pitch_ratio = 1.0;
            }
        }
    }
    
    fclose(file);
    printf("Song loaded successfully: %d channels, %d rows, BPM: %d\n",
           song->num_channels, song->num_rows, song->bpm);
    
    return 1;
}

// ---- Save song to file ----
int save_song(Song* song, const char* filename) {
    FILE* file = fopen(filename, "w");
    if (!file) {
        printf("Error: Could not create file %s\n", filename);
        return 0;
    }
    
    printf("Saving song to %s...\n", filename);
    
    // Write header
    fprintf(file, "CTracker Song\n");
    fprintf(file, "BPM: %d\n", song->bpm);
    fprintf(file, "Rows: %d\n", song->num_rows);
    fprintf(file, "Channels: %d\n", song->num_channels);
    fprintf(file, "Loop: %d %d %d\n", song->loop_enabled, song->loop_start, song->loop_end);
    
    // Write each cell
    for (int ch = 0; ch < song->num_channels; ch++) {
        for (int row = 0; row < song->num_rows; row++) {
            Cell* c = &song->channels[ch].cells[row];
            const char* note_name = midi_to_note_name(c->note);
            
            fprintf(file, "%d %d %s %s\n", 
                   c->note, c->original_note, note_name, c->sample);
        }
    }
    
    fclose(file);
    printf("Song saved successfully.\n");
    
    return 1;
}

// ---- Save song to WAV file (export) ----
void export_to_wav(Song* song) {
    char filename[256];
    
    printf("Enter WAV filename (e.g., song.wav): ");
    fgets(filename, sizeof(filename), stdin);
    filename[strcspn(filename, "\n")] = 0;
    
    if (strlen(filename) == 0) {
        strcpy(filename, "song.wav");
    }
    
    printf("Exporting to %s...\n", filename);
    
    if (save_song_to_wav(song, filename)) {
        printf("Export successful!\n");
    } else {
        printf("Export failed!\n");
    }
    
    printf("Press Enter to continue...");
    getchar();
}

// ---- Load song from file ----
void load_song_from_file(Song* song) {
    char filename[256];
    
    printf("Enter song filename to load: ");
    fgets(filename, sizeof(filename), stdin);
    filename[strcspn(filename, "\n")] = 0;
    
    if (load_song(song, filename)) {
        printf("Song loaded successfully!\n");
    } else {
        printf("Failed to load song!\n");
    }
    
    printf("Press Enter to continue...");
    getchar();
}

// ---- Save song to file ----
void save_song_to_file(Song* song) {
    char filename[256];
    
    printf("Enter song filename to save: ");
    fgets(filename, sizeof(filename), stdin);
    filename[strcspn(filename, "\n")] = 0;
    
    if (strlen(filename) == 0) {
        strcpy(filename, "song.ctrack");
    }
    
    if (save_song(song, filename)) {
        printf("Song saved successfully!\n");
    } else {
        printf("Failed to save song!\n");
    }
    
    printf("Press Enter to continue...");
    getchar();
}

// ---- Main ----
int main() {
    Song song = {0};
    song.num_channels = MAX_CHANNELS;  // 8 channels
    song.num_rows = MAX_ROWS;
    song.bpm = 120;  // Default BPM value
    song.loop_enabled = 0;
    song.loop_start = 0;
    song.loop_end = MAX_ROWS - 1;
    
    // Initialize note durations and pitch ratios
    int note_duration = get_note_duration_ms(&song);
    for (int ch = 0; ch < song.num_channels; ch++) {
        for (int row = 0; row < song.num_rows; row++) {
            song.channels[ch].cells[row].duration_ms = note_duration;
            song.channels[ch].cells[row].original_note = 60; // Default C4
            song.channels[ch].cells[row].pitch_ratio = 1.0;
        }
    }

    int cursor_row = 0, cursor_channel = 0;
    int running = 1;

    while (running) {
        draw_tty(&song, cursor_row, cursor_channel);
        printf("\nControls:\n");
        printf("WASD - navigation\n");
        printf("E - edit cell\n");
        printf("P - play entire track\n");
        printf("R - play current row\n");
        printf("B - change BPM (current: %d)\n", song.bpm);
        printf("L - set loop points\n");
        printf("F - save song to file\n");
        printf("G - load song from file\n");
        printf("X - export to WAV file\n");
        printf("Q - quit\n");
        printf("\nNote format: NoteName+Octave (e.g., C4, A#3, F-1)\n");
        printf("Use '---' for rest/silence\n");
        printf("Samples will be pitch-shifted to match the note!\n");
        
        int c = getch();
        switch(c) {
            case 'q': 
                running = 0; 
                break;
            case 'w': 
                if(cursor_row > 0) cursor_row--; 
                break;
            case 's': 
                if(cursor_row < song.num_rows - 1) cursor_row++; 
                break;
            case 'a': 
                if(cursor_channel > 0) cursor_channel--; 
                break;
            case 'd': 
                if(cursor_channel < song.num_channels - 1) cursor_channel++; 
                break;
            case 'e': 
                edit_cell(&song, cursor_row, cursor_channel); 
                break;
            case 'p': 
                play_song(&song); 
                break;
            case 'r':
                play_current_row(&song, cursor_row);
                break;
            case 'b': 
                change_bpm(&song); 
                break;
            case 'l':
                set_loop(&song);
                break;
            case 'f':
                save_song_to_file(&song);
                break;
            case 'g':
                load_song_from_file(&song);
                break;
            case 'x':
                export_to_wav(&song);
                break;
        }
    }

    // Stop all playback before exit
    for (int i = 0; i < MAX_CHANNELS; i++) {
        pthread_mutex_lock(&audio_mutex);
        tone_threads[i].active = 0;
        sample_threads[i].active = 0;
        pthread_mutex_unlock(&audio_mutex);
    }
    
    SDL_Delay(100);
    
    return 0;
}

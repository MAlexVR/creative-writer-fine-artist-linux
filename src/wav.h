#ifndef WAV_H
#define WAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* WAVE files and wave formats: PCMWAVEFORMAT / WAVEFORMATEX parsing, RIFF chunk walking, and decoding to
 * interleaved signed 16 bit samples. PCM 8 bit (unsigned) and 16 bit, mono or stereo, and Microsoft ADPCM
 * (format tag 2, 4 bit): the 127 sound resources of WRITER.SND and the 137 of ARTIST.SND are ADPCM at
 * 11025 Hz mono, and the Win16 mapper (ACM) converted them for the sound card. */

#define WAVE_FORMAT_PCM 1
#define WAVE_FORMAT_ADPCM 2

typedef enum { WAV_OK, WAV_E_SHORT, WAV_E_RIFF, WAV_E_NOFMT, WAV_E_NODATA } wav_err;

typedef struct {
    uint16_t tag, channels;
    uint32_t rate, avg_bytes;
    uint16_t block_align, bits;
    uint16_t spb;           /* ADPCM: samples per channel in a block */
    uint16_t ncoef;         /* ADPCM: coefficient pairs, at most 7 */
    int16_t coef[7][2];
} wav_format;

/* A format structure as the guest passes it: 16 bytes of PCMWAVEFORMAT, plus cbSize and the extension for
 * tag 2 (samples per block, coefficient count, the pairs). */
wav_err wav_format_parse(const uint8_t *fmt, size_t len, wav_format *out);
/* True for what wav_decode handles (a rate of 1..96000 Hz, one or two channels). */
bool wav_format_supported(const wav_format *f);

/* The fmt and data chunks of a RIFF/WAVE image; *data points into file. */
wav_err wav_parse(const uint8_t *file, size_t len, wav_format *fmt, const uint8_t **data, size_t *data_len);

/* Decodes data to *out (malloc, interleaved s16, f->channels per frame); returns the frames, 0 when there is
 * nothing to decode or memory ran out (*out is then NULL). A partial last block is decoded as far as it goes. */
size_t wav_decode(const wav_format *f, const uint8_t *data, size_t len, int16_t **out);

#endif

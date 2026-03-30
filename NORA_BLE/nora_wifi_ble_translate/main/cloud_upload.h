/*
 * cloud_upload.h — Google Cloud Storage upload + Speech-to-Text transcription
 *
 * CONFIGURATION — edit these before building:
 *   CLOUD_GCS_BUCKET   : GCS bucket name (bucket is public — no auth needed for upload)
 *   SPEECH_API_KEY     : Google Cloud Speech-to-Text API key
 *                        Enable "Cloud Speech-to-Text API" in GCP Console.
 *
 * UART RESPONSES sent back to STM32:
 *   "UPLOAD:OK:filename.wav\n"   — GCS upload succeeded
 *   "UPLOAD:FAIL:filename.wav\n" — GCS upload failed
 *   "TRANSCRIPT:<text>\n"        — transcription result (handed to command_router)
 *   "STT:FAIL\n"                 — transcription failed or no speech detected
 */

#ifndef CLOUD_UPLOAD_H
#define CLOUD_UPLOAD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── User configuration ──────────────────────────────────────────────────── */
#define CLOUD_GCS_BUCKET   "my-music-sightsys"   /* public bucket — no auth required for upload */
#define SPEECH_API_KEY     "AIzaSyB-ClvuzNPEIwXGz_NSb0yApeRye-Gt13g"

/* Maximum transcript length returned from Speech-to-Text. */
#define CLOUD_TRANSCRIPT_MAX    128u

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * CloudUpload_Init — must be called once after WiFi is connected.
 * Sets up the TLS configuration used by all HTTPS requests.
 */
void CloudUpload_Init(void);

/*
 * CloudUpload_UploadWav — upload a WAV buffer to Google Cloud Storage.
 *
 *   filename : object name to create in the bucket (e.g. "REC_001.wav")
 *   data     : pointer to WAV file bytes (complete file including 44-byte header)
 *   len      : total byte count
 *
 * Returns true on HTTP 200/201, false on any error.
 * Sends "UPLOAD:OK:<filename>\n" or "UPLOAD:FAIL:<filename>\n" to STM32.
 */
bool CloudUpload_UploadWav(const char *filename, const uint8_t *data, size_t len);

/*
 * CloudUpload_Transcribe — call Google Cloud Speech-to-Text on a file
 * already present in the GCS bucket.
 *
 *   filename       : object name in the bucket (e.g. "REC_001.wav")
 *   out_transcript : caller-allocated buffer; receives null-terminated text
 *   maxLen         : size of out_transcript (recommend CLOUD_TRANSCRIPT_MAX)
 *
 * Returns true if a non-empty transcript was obtained.
 * Sends "STT:FAIL\n" to STM32 on failure.
 */
bool CloudUpload_Transcribe(const char *filename,
                            char       *out_transcript,
                            size_t      maxLen);

#endif /* CLOUD_UPLOAD_H */

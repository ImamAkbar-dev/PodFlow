#!/usr/bin/env bash
set -euo pipefail

# ==============================================================================
# Script Generator Audio WAV Canonical 44-Byte (Session 1)
# Format: PCM 16-bit, 44100 Hz, Stereo (4 bytes per audio frame)
# ==============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"

echo "[+] Generating canonical WAV test files in ${SCRIPT_DIR}..."

# Menggunakan Python wave module agar header persis 44 byte (tanpa metadata LIST dari ffmpeg)
python3 - << 'EOF'
import wave
import math
import struct

def generate_wav(filename, num_pcm_frames, freq=440.0):
    sample_rate = 44100
    num_channels = 2
    sample_width = 2  # 16-bit PCM = 2 bytes per sample
    
    with wave.open(filename, 'wb') as wav_file:
        wav_file.setnchannels(num_channels)
        wav_file.setsampwidth(sample_width)
        wav_file.setframerate(sample_rate)
        
        amplitude = 10000
        buffer = bytearray()
        
        for i in range(num_pcm_frames):
            t = float(i) / sample_rate
            value = int(amplitude * math.sin(2.0 * math.pi * freq * t))
            packed_frame = struct.pack('<hh', value, value)
            buffer.extend(packed_frame)
            
            if len(buffer) >= 65536:
                wav_file.writeframes(buffer)
                buffer.clear()
                
        if buffer:
            wav_file.writeframes(buffer)

# 1. sample_30s.wav (5,292,044 bytes)
generate_wav("sample_30s.wav", 1323000, 440.0)

# 2. sample_5m.wav (52,920,044 bytes)
generate_wav("sample_5m.wav", 13230000, 440.0)

# 3. sample_tail.wav (418,112 bytes - menguji partial chunk di EOF)
generate_wav("sample_tail.wav", 104517, 880.0)

EOF

echo "[+] Verifying generated WAV headers and sizes..."

verify_wav() {
    local file="$1"
    local expected_size="$2"
    
    if [ ! -f "$file" ]; then
        echo "[-] ERROR: File $file does not exist!"
        exit 1
    fi
    
    local actual_size
    actual_size=$(stat -c%s "$file")
    
    if [ "$actual_size" -ne "$expected_size" ]; then
        echo "[-] ERROR: $file size mismatch! Expected: $expected_size, Got: $actual_size"
        exit 1
    fi
    
    # Cek Tag RIFF (byte 0-3)
    local riff_tag
    riff_tag=$(xxd -p -l 4 "$file")
    if [ "$riff_tag" != "52494646" ]; then
        echo "[-] ERROR: $file invalid RIFF header!"
        exit 1
    fi
    
    # Cek Tag WAVE (byte 8-11)
    local wave_tag
    wave_tag=$(xxd -s 8 -l 4 -p "$file")
    if [ "$wave_tag" != "57415645" ]; then
        echo "[-] ERROR: $file invalid WAVE header!"
        exit 1
    fi
    
    # Cek Tag data (byte 36-39)
    local data_tag
    data_tag=$(xxd -s 36 -l 4 -p "$file")
    if [ "$data_tag" != "64617461" ]; then
        echo "[-] ERROR: $file invalid data chunk header at byte 36!"
        exit 1
    fi

    echo "  [OK] $file ($actual_size bytes) - Header 44 B verified."
}

verify_wav "sample_30s.wav" 5292044
verify_wav "sample_5m.wav" 52920044
verify_wav "sample_tail.wav" 418112

echo "[+] ALL WAV TEST FILES GENERATED AND VERIFIED SUCCESSFULLY!"
# stb_vorbis: the only extra audio dependency; WAV decoding uses SDL3.
FetchContent_Declare(stb_vorbis
    URL https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_vorbis.c
    URL_HASH SHA256=4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(stb_vorbis)

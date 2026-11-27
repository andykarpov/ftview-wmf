#pragma once
// Общие поля AVI. В видеобанке это его рабочие переменные (макросы avi_* в
// avi_index.c); банку изображений (far_call) и банку списка копия структуры
// передаётся в AVI_FAR.
typedef struct
{
  u8 op;              // AVI_OP_*
  u8 ok;
  u16 pcm_rate;
  u32 movi_start;     // позиция после FOURCC 'movi'
  u32 movi_end;       // конец LIST movi
  u32 total, rate, scale, bytes, pcm_total;
  u32 target;         // AVI_OP_SEEK: кадр назначения
  u32 seek_pos, seek_frame, seek_audio;
  u32 write;          // указатель записи media FIFO
  u32 period;         // avih: микросекунд на кадр
  u16 width, height;  // avih: размер кадра
  // Размер файла ролика. filesize (#BFE6) у каждого банка кода свой: после
  // выбора в списке у банка изображений там остался бы прежний ролик.
  u32 size;
  // File I/O of the AVI (main.c io_read): io 1 — the SD reader of the I/O
  // bank (sdr.c) at sector disk, 0 — the stream of WC (LOAD512 / GIPAGPL).
  u32 disk;
  u8 io;
  u8 msg, code;       // AVI_OP_MSG: window (AVI_MSG_*), reason
  // The frame on the screen of the FT812 mode (avi_view of the image bank,
  // the video bank has no room for it): size, BITMAP_TRANSFORM_A / E.
  u16 view_w, view_h;
  u32 ta, te;
} AVI_SHARED;

#define AVI_OP_SEEK 1     // точка входа для target (нули — с начала файла);
                          // ok 2 — чтение idx1 сдвинуло позицию диска
#define AVI_OP_HEADER 2   // заголовок в FIFO, поток на seek_pos
#define AVI_OP_PROBE 3    // разбор заголовка: ok 1 — покадровый режим, 2 — сбой
// Taken over from the video list bank of WC Improved (not built here: it
// reads the directory through WildDOS internals).
#define AVI_OP_MJPEG 5    // codec by the start of the file in fs_buf: ok 0 — another
#define AVI_OP_MSG 6      // message window msg (reason code) until a key

#define AVI_MSG_CODEC 1   // «Unsupported video codec»
#define AVI_MSG_FILE 2    // «Cannot display file»
#define AVI_MSG_NOFT 4    // «No VDAC2 detected!»
// Копия полей для банков изображений и списка: свободный хвост первой
// страницы буфера.
#define AVI_FAR ((AVI_SHARED*)0xFF00)

// Раскладка RAM_G: слово результата декодера над двумя кадрами, кольцо PCM
// FT812 с #C0000. Нужна и разбору заголовка в банке изображений.
#define AVI_RESULT 0xDFFFCUL
#define PCM_BASE 0xC0000UL

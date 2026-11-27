
#include <stdio.h>
#include <string.h>
#include <defs.h>
#include <sdklib.h>
#include <ft812.h>
#include <ft812lib.h>
#include <tslib.h>
#include <wc_api.h>
#include "tsconf.h"

#include <build_id.h>
#include <wc_api.c>
#include "sdr.h"
#include "io_bank.h"
#include "avi_shared.h"

typedef struct
{
  u8 sig[3];
  u8 type;
  u16 xs;
  u16 ys;
} DXP;

typedef struct
{
  u16 sig;
  u16 len;
  u8 prec;
  u8 ys[2];
  u8 xs[2];
} SOF;

typedef struct
{
  u32 sig;
  u8 xs[4];
  u8 ys[4];
  u8 bits;
  u8 ctype;
} IHDR;

typedef struct
{
  u32 len;
  u32 sig;
} tRNS;

#define FS_BUF_SIZE 0x1000
// Вторая страница выделяется самим WC по заголовку WMF и отображается в
// #C000 через API 0. Это собственная память плагина, не буфер интерфейса WC.
// Чтение восьми секторов за вызов уменьшает накладные расходы LOAD512/FAT.
// Код, cmdl и состояние остаются в #8000..#BFFF, отдельно от файловых данных.
__at (0xC000) u8 fs_buf[0x4000];
u8 cmdl[0x180];

// У плагина нет стандартного CRT: инициализируем состояние явно при входе.
// Запись через приведение указателя к const запрещена правилами C.
u8 state_ft, graphics_active;
// Единственные общие данные двух банков кода. При переходе через API 79
// адреса не меняются; обычные глобальные переменные новый банк создаёт заново.
// Диапазон закреплён проверками сборки и не пересекается с DATA/буфером #C000.
__at (0xBFE0) u16 ret_sp;
__at (0xBFE2) u16 ret_ix;
__at (0xBFE4) u16 ret_iy;
__at (0xBFE6) u32 filesize;
__at (0xBFEA) u8 file_ext;
__at (0xBFEB) u8 call_type;
// Банк изображений загрузил драйвер General Sound: звук AVI идёт на GS.
// При любом выходе видеобанк возвращает GS в ПЗУ (см. view_cleanup).
__at (0xBFEC) u8 gs_loaded;
// Для AVI VDAC2 инициализирует банк изображений: state_ft в битах 0..1,
// биты 2..4 — этап отказа загрузки GS (gs_stage, 0 — драйвер работает),
// бит 5 — пиксели режима вдвое шире своей высоты (режим 10),
// бит 7 — видеосреда WC уже изменена и требует восстановления.
__at (0xBFED) u8 ft_state;
// The screen of the FT812 mode (vdac2_init of the image bank, the option
// -mode of wc.ini): the visible size in pixels.
__at (0xBFEE) u16 scr_w;
__at (0xBFF0) u16 scr_h;
void wc_video_bank();
// far_entry of bank page with a parameter block in #C000; back to this bank
// (the gate _far_gate of crt0 takes the page to return to in C).
#ifdef FTVIEW_VIDEO_BANK
#define BANK_PAGE 9
#else
#define BANK_PAGE 0
#endif
void far_call(u8 page, void *param) __naked
{
  page; param;
  __asm
    ld c, #BANK_PAGE
    jp _far_gate
  __endasm;
}

// Признак неудачной загрузки текущего файла. Сбрасывается при входе в плагин;
// нужен для общего выхода в WC после ошибки чтения, формата или сопроцессора.
u8 view_failed;
// Место отказа плеера: номер видно в окне сообщения («Reason NN»). Редкий
// сбой у пользователя так разбирается по номеру, а не по догадкам.
u8 fail_at;
void fail(u8 at)
{
  fail_at = at;
  view_failed = 1;
}
#define VIEW_TOO_LARGE 2
#define VIEW_CODEC 3        // AVI с чужим кодеком видео

// left — число ещё не переданных байтов файла. Читаем не больше fs_buf,
// округляя только запрос к WC до целых секторов. Возвращаем true лишь если
// LOAD512 продвинул указатель на весь запрошенный объём. Последний неполный
// сектор допустим, но его байты за EOF потребитель не должен передавать FT812.
// File I/O: sectors into fs_buf (#C000 of the mapped buffer page). An AVI
// with the SD reader (avi.io) reads at avi.disk: the image bank itself, the
// video bank through it (AVI_OP_READ); otherwise the stream of WC (LOAD512).
// The reader is in the I/O bank: it needs buffer page 1 in #C000 (its state
// and the block), the data go by DMA into the page that was mapped. Page 1
// stays mapped after the call.
extern AVI_SHARED avi;
bool io_read(u8 sectors)
{
  u8 page;
  if (!avi.io)
    return wc_load512(fs_buf, sectors) == (u16)fs_buf + ((u16)sectors << 9);
  page = *(u8*)_PAGE3;
  wc_map_buffer(1);
  IO_FAR->op = IO_READ;
  IO_FAR->fsec = avi.disk;
  IO_FAR->n = sectors;
  IO_FAR->page = page;
  IO_FAR->offs = 0;
  far_call(IO_PAGE, IO_FAR);
  if (!IO_FAR->ok) return false;
  avi.disk += sectors;
  return true;
}

bool read_chunk(u32 left)
{
  u16 count = min(left, FS_BUF_SIZE);
  u8 sectors = (count + 511) >> 9;
  if (!count || *(volatile u8*)_ABT || !io_read(sectors))
  {
    fail(1);
    return false;
  }
  return true;
}

// Ждём wanted свободных байтов в командном FIFO. Значение 0xFFC означает,
// что очередь пуста; потоковое видео требует ещё и команды-барьера (см. AVI).
// Младшие ненулевые биты — ошибка сопроцессора.
// Ограничение 150 тиков WC прерывает зависший декодер. Независимый счётчик
// опросов гарантирует выход и при остановленном таймере. Esc отменяет ожидание.
// При неудаче сбрасываем сопроцессор, чтобы следующий файл мог открыться.
bool fifo_wait(u16 wanted)
{
  u8 tick = *(volatile u8*)_TMN;
  u16 polls = 0xFFFF;
  do
  {
    u16 space = ft_rreg16(FT_REG_CMDB_SPACE);
    if (*(volatile u8*)_ABT || (space & 3)) break;
    if (space >= wanted && space <= 0xFFC) return true;
  } while (--polls && (u8)(*(volatile u8*)_TMN - tick) < 150);
  fail(2);
  ft_cp_reset();
  return false;
}

// Передаём count полезных байтов в FIFO, дожидаясь свободного места порциями.
// FT812 требует длину, кратную четырём: хвост последней порции зануляем,
// иначе в поток попадают старые байты буфера. У data должны быть доступны
// до трёх дополнительных байтов. Нулевую длину в функцию SDK не передаём.
bool fifo_send(u8 *data, u16 count)
{
  u16 padded = (count + 3) & 0xFFFC;
  u16 tail = count;
  // Все вызывающие передают буфер с запасом до кратной четырём границы.
  while (tail < padded) data[tail++] = 0;
  while (padded)
  {
    // Ждём места для всей порции. Повторное чтение SPACE с маской могло
    // превратить код ошибки 0xFFF в допустимые 0xFFC между двумя опросами.
    u16 part = min(padded, 0xFFC);
    if (!fifo_wait(part)) return false;
    // OTIR тратит 21 такт Z80 на байт: при штатных 7 МГц WC этого мало
    // для AVI около 380 КБ/с, и звуковое кольцо повторяет старые сэмплы.
    // Данные и cmdl лежат в разных собственных страницах; DMA передаёт ровно
    // part байтов (кратно четырём), не захватывая хвост последнего сектора.
    ft_load_ram_dma(data, *(u8*)((u16)data >= 0xC000 ? _PAGE3 : _PAGE2),
                    FT_REG_CMDB_WRITE, part);
    data += part;
    padded -= part;
  }
  return true;
}

// Отправляем команды, накопленные SDK в cmdl, через тот же проверяемый путь,
// что и данные. Счётчик SDK измеряется в 32-битных словах, не в байтах.
bool fifo_flush()
{
  u16 count = ft_ccmdp << 2;
  ft_ccmdp = 0;
  return fifo_send((u8*)ft_ccmdb, count);
}

#include "gs_zx.c"

// Остановка звука FT812 (кольцо PCM без GS и звук штатного PLAYVIDEO).
// Запись в REG_PLAYBACK_PLAY запускает воспроизведение при любом значении
// (руководство FT81x, Register Definition 26), поэтому прежний «PLAY = 0» не
// останавливал звук, а перезапускал кольцо с LOOP = 1: последний фрагмент
// играл по кругу до сброса. Останавливает пустой отрезок: длина 0 и запуск
// (там же, Code snippet 7). Только если звук идёт (PLAY читается 1): при
// звуке на GS звуковой блок не запускался, и лишний запуск ни к чему —
// эмулятор Bridgetek в Unreal без звукового устройства Windows на любом
// запуске звука падает.
void ft_pb_stop()
{
  if (!ft_rreg8(FT_REG_PLAYBACK_PLAY)) return;
  ft_wreg32(FT_REG_PLAYBACK_LENGTH, 0);
  ft_wreg8(FT_REG_PLAYBACK_LOOP, 0);
  ft_wreg8(FT_REG_PLAYBACK_PLAY, 1);
}

// Единый выход нужен и при ошибке, и при PgUp/PgDn: остановить видео/звук,
// снять выбор SPI, вернуть палитру и теневые видеорегистры WC. API 64 пишет
// палитру по #1000, поэтому сначала отображаем свободную видеостраницу API 80;
// нельзя выполнять GEDPL поверх страницы файловой системы или плагина.
void view_cleanup()
{
#ifdef FTVIEW_VIDEO_BANK
  // Драйвер GS отдаёт управление ПЗУ: иначе другие программы не получат
  // штатных команд GS. Выполняется и тогда, когда VDAC2 не найден.
  if (gs_loaded)
  {
    (void)gs_data;  // непрочитанный ответ драйвера не должен задержать выход
    gs_cmd(GS_EXIT);
    gs_loaded = 0;
  }
#endif
  if (!graphics_active) return;
  ft_cp_reset();
  ft_pb_stop();
  ft_wreg8(FT_REG_INT_EN, 0);
  ft_spi_unsel();
  wc_video_clock(0);
  __asm
    push ix
    xor a
    ex af, af
    ld a, #80
    call _WCAPI
    xor a
    ex af, af
    ld a, #_MNGV_PL
    call _WCAPI
    pop ix
  __endasm;
  graphics_active = state_ft = 0;
}

// --- Extensions ---------------------------
enum
{
  EXT_JPG,
  EXT_PNG,
  EXT_DLS,
  EXT_DXP,
  EXT_AVI,
};

// --- Windows ------------------------------
// В видеобанке окон нет: их показывает банк списка (list_error) — здесь на
// них уже не хватает места. У банка списка свои копии (avi_list.c).
#ifndef FTVIEW_VIDEO_BANK
const WC_TX_WINDOW err_no_ft =
{
  /* window with header and text   */  WC_WIND_HDR_TXT,
  /* cursor color mask             */  0,
  /* X,Y (position)                */  24, 10,
  /* W,H (size)                    */  32, 5,
  /* paper/ink (window color)      */  0x2F,
  /* -reserved-                    */  0,
  /* window restore buffer address */  0,
  /* separators                    */  0, 0,
  /* header text                   */  "\x0E" " Error! ",
  /* footer text                   */  "",
  /* window text                   */  "\x0E\x0C\x01" "No VDAC2 detected!",
};

#endif

// Сообщение остаётся на текстовом экране WC; повреждённый файл не должен
// молча показывать последний кадр предыдущего просмотра.
// Коды текста WC: 0E центрирует строку, меряя её до CR или конца текста;
// 0C N — вниз на N строк; CR — в начало той же строки. Без перевода строки
// вторая строка печаталась поверх первой («UnsuFT812 image memory: 1 MiB.rge.»).
// В видеобанке окон сообщений нет: их показывает банк списка (list_error) —
// в видеобанке на них уже не хватает места.
#ifndef FTVIEW_VIDEO_BANK
const WC_TX_WINDOW err_file =
{
  WC_WIND_HDR_TXT, 0, 19, 10, 42, 7, 0x2F, 0, 0, 0, 0,
  "\x0E" " Cannot display file ", "",
  "\x0E\x0C\x01" "Unsupported, damaged or too large.\r"
  "\x0E\x0C\x02" "FT812 image memory: 1 MiB."
};

const WC_TX_WINDOW err_size =
{
  WC_WIND_HDR_TXT, 0, 19, 10, 42, 7, 0x2F, 0, 0, 0, 0,
  "\x0E" " Error! ", "",
  // Текст WC начинается с внутренней строки 1. Смещаем на 2 к середине
  // окна высотой 7; код 0E вычисляет горизонтальный центр по длине строки.
  "\x0C\x02\x0E" "Image too large"
};
#endif

// --- FT812 --------------------------------
// The FT812 is set up by the I/O bank (io_bank.c: the mode table of the SDK
// does not fit here), in the mode of the option -mode of wc.ini (wc_h.c).
// Buffer page 1 must be in #C000 (IO_FAR).
#ifndef FTVIEW_VIDEO_BANK
bool vdac2_init()
{
  IO_FAR->op = IO_FT_INIT;
  IO_FAR->n = wc_param(1);
  far_call(IO_PAGE, IO_FAR);
  if (IO_FAR->ok) graphics_active = 1;
  scr_w = IO_FAR->offs;
  scr_h = (u16)IO_FAR->fsec;
  return IO_FAR->ok == 2;
}

#endif

#ifndef FTVIEW_VIDEO_BANK
// Буферный разбор заголовка: JPEG может иметь EXIF/APP-сегменты длиннее
// 4 КиБ. Содержимое сегментов пропускаем по длине, а не ищем FF Cx внутри
// произвольных данных: FF C4, например, обозначает таблицу Хаффмана, не SOF.
u32 header_left;
u16 header_pos, header_size;
u16 image_x, image_y;

u8 header_byte()
{
  // После отказа чтения avi_word()/разбор маркера ещё могут запросить
  // остальные байты поля. Не повторяем LOAD512 поверх ошибочного потока.
  if (view_failed) return 0;
  if (header_pos == header_size)
  {
    if (!header_left || !read_chunk(header_left))
    { view_failed = 1; return 0; }
    header_size = min(header_left, FS_BUF_SIZE);
    header_pos = 0;
    header_left -= header_size;
  }
  return fs_buf[header_pos++];
}

// Считываем RIFF little-endian независимо от выравнивания поля в буфере.
u32 avi_word()
{
  u32 value = header_byte();
  value |= (u32)header_byte() << 8;
  value |= (u32)header_byte() << 16;
  value |= (u32)header_byte() << 24;
  return value;
}
#endif

#ifndef FTVIEW_VIDEO_BANK
// The message windows of the AVI player, from the list bank of WC Improved.
// The last two digits of err_avi are the place the player gave up (fail_at).
const WC_TX_WINDOW err_codec =
{
  WC_WIND_HDR_TXT, 0, 19, 10, 42, 7, 0x2F, 0, 0, 0, 0,
  "\x0E" " Cannot play video ", "",
  "\x0E\x0C\x01" "Unsupported video codec.\r"
  "\x0E\x0C\x02" "FTView plays MJPEG AVI."
};

const WC_TX_WINDOW err_avi =
{
  WC_WIND_HDR_TXT, 0, 19, 10, 42, 8, 0x2F, 0, 0, 0, 0,
  "\x0E" " Cannot display file ", "",
  "\x0E\x0C\x01" "Unsupported, damaged or too large.\r"
  "\x0E\x0C\x02" "FT812 image memory: 1 MiB.\r"
  "\x0E\x0C\x03" "Build " BUILD_ID "   Reason 00"
};

void avi_message(u8 msg, u8 code)
{
  const WC_TX_WINDOW *window = msg == AVI_MSG_CODEC ? &err_codec :
                               msg == AVI_MSG_NOFT ? &err_no_ft : &err_avi;
  if (msg == AVI_MSG_FILE)
  {
    // The text is in the RAM of the bank page: the digits are set in place.
    char *s = (char*)err_avi.wnd_txt;
    while (*s) ++s;
    s[-2] = '0' + code / 10;
    s[-1] = '0' + code % 10;
  }
  wc_api_u16(_PRWOW, (u16)window);
  wc_api__bool(_USPO);
  wc_api__bool(_NUSP);
  wc_api_u16(_RRESB, (u16)window);
}
#endif

#include "avi_index.c"

#ifndef FTVIEW_VIDEO_BANK
u16 header_word()
{
  u16 hi = header_byte();
  return (hi << 8) | header_byte();
}

// Разрешаем только форматы, поддержанные CMD_LOADIMAGE FT81x: baseline JPEG
// (8 бит, один или три компонента) и PNG 8 бит без чересстрочной развёртки.
// До передачи сжатых данных проверяем, что результат поместится в RAM_G
// (1 МиБ), а ширина/высота — в 11-битные поля BITMAP_SIZE_H/LAYOUT_H.
bool image_header(u8 is_jpg)
{
  u8 pixel_bytes = 2;
  u16 extra = 0;
  header_left = filesize;
  header_pos = header_size = 0;
  if (is_jpg)
  {
    u8 marker;
    u16 length;
    if (header_word() != 0xFFD8) return false;
    for (;;)
    {
      if (view_failed || header_byte() != 0xFF) return false;
      do { marker = header_byte(); } while (marker == 0xFF && !view_failed);
      if (view_failed || marker == 0 || marker == 0xD8 || marker == 0xD9 ||
          marker == 0xDA || (marker >= 0xD0 && marker <= 0xD7)) return false;
      length = header_word();
      if (view_failed || length < 2) return false;
      if (marker == 0xC0)
      {
        u8 components;
        if (length < 11 || header_byte() != 8) return false;
        image_y = header_word();
        image_x = header_word();
        components = header_byte();
        if ((components != 1 && components != 3) || length != 8 + 3 * components)
          return false;
        pixel_bytes = (components == 1) ? 1 : 2;
        break;
      }
      // Остальные SOF — progressive/lossless/extended JPEG. Таблицы DHT,
      // DAC и маркер JPG сюда не относятся и могут быть пропущены по длине.
      if (marker >= 0xC0 && marker <= 0xCF &&
          marker != 0xC4 && marker != 0xC8 && marker != 0xCC) return false;
      length -= 2;
      while (length-- && !view_failed) header_byte();
    }
  }
  else
  {
    u8 type;
    if (filesize < 33 || !read_chunk(filesize) ||
        memcmp(fs_buf, "\x89PNG\x0D\x0A\x1A\x0A", 8) ||
        memcmp(fs_buf + 8, "\0\0\0\x0D" "IHDR", 8) ||
        fs_buf[16] || fs_buf[17] || fs_buf[20] || fs_buf[21] ||
        fs_buf[24] != 8 || fs_buf[26] || fs_buf[27] || fs_buf[28]) return false;
    image_x = ((u16)fs_buf[18] << 8) | fs_buf[19];
    image_y = ((u16)fs_buf[22] << 8) | fs_buf[23];
    type = fs_buf[25];
    if (type == 0) pixel_bytes = 1;
    else if (type == 3) { pixel_bytes = 1; extra = 512; }
    else if (type != 2 && type != 6) return false;
  }
  if (view_failed || !image_x || !image_y) return false;
  if (image_x > 2047 || image_y > 2047 ||
      (u32)image_x * image_y * pixel_bytes + extra > 0x100000UL)
  { view_failed = VIEW_TOO_LARGE; return false; }
  return wc_rewind();
}

void show_jpg_png(u8 is_jpg)
{
  u32 left = filesize;
  if (!image_header(is_jpg))
  { if (!view_failed) view_failed = 1; return; }
  wc_vmode(0x87);
  ft_ccmd_start(cmdl);
  ft_Dlstart();
  ft_Clear(1, 1, 1);
  // CMD_LOADIMAGE сам выставляет формат и адрес палитры. Ручное угадывание
  // по tRNS ломало индексированные PNG с несколькими прозрачными цветами.
  // SWAP выполняем после декодирования, не показывая старое содержимое RAM_G.
  ft_LoadImage(FT_RAM_G, 0);
  if (!fifo_flush()) return;
  while (left)
  {
    u16 count = min(left, FS_BUF_SIZE);
    if (!read_chunk(left) || !fifo_send(fs_buf, count)) return;
    left -= count;
  }
  if (!fifo_wait(0xFFC)) return;
  ft_Begin(FT_BITMAPS);
  ft_Vertex2ii((scr_w - min(scr_w, image_x)) >> 1,
              (scr_h - min(scr_h, image_y)) >> 1, 0, 0);
  ft_Display();
  ft_ccmd(FT_CCMD_SWAP);
  if (fifo_flush()) fifo_wait(0xFFC);
}

void show_dxp()
{
  u32 sz, o;
  u16 xo, yo;
  DXP *d;

  // Проверяем длину до unsigned-вычитания: размер 0..7 раньше превращался
  // в почти 4 ГиБ. Размеры DXT1 кратны четырём; два массива занимают xs*ys/2.
  if (filesize < sizeof(DXP) || !read_chunk(filesize))
  { view_failed = 1; return; }
  sz = filesize - sizeof(DXP);
  d = (DXP*)fs_buf;
  if (memcmp(d->sig, "DXP", 3) || d->type > 1 ||
      !d->xs || !d->ys || ((d->xs | d->ys) & 3) ||
      d->xs > 2047 || d->ys > 2047 || !sz)
  { view_failed = 1; return; }
  o = (u32)d->xs * ((u32)d->ys >> 2);
  if (o > 0x80000UL || (!d->type && sz != o * 2))
  { view_failed = 1; return; }
  xo = (scr_w - min(scr_w, d->xs)) >> 1;
  yo = (scr_h - min(scr_h, d->ys)) >> 1;
  wc_vmode(0x87);

  ft_ccmd_start(cmdl);
  ft_Dlstart();
  ft_Clear(1, 1, 1);
  // ft_ccmd(ft_SaveContext());

  ft_BitmapHandle(0);
  ft_SetBitmap(FT_RAM_G + o, FT_L2, d->xs, d->ys);
  ft_BitmapHandle(1);
  ft_SetBitmap(FT_RAM_G, FT_RGB565, d->xs >> 2, d->ys >> 2);
  ft_BitmapSize(FT_NEAREST, FT_BORDER, FT_BORDER, d->xs, d->ys);

  ft_Begin(FT_BITMAPS);
  ft_ColorA(255);
  ft_BlendFunc(FT_ONE, FT_ZERO);
  ft_Vertex2ii(xo, yo, 0, 0);

  ft_ColorMask(1, 1, 1, 0);
  ft_BitmapTransformA(64);
  ft_BitmapTransformE(64);
  ft_BlendFunc(FT_ONE_MINUS_DST_ALPHA, FT_ZERO);
  ft_Vertex2ii(xo, yo, 1, 0);
  ft_BlendFunc(FT_DST_ALPHA, FT_ONE);
  ft_Vertex2ii(xo, yo, 1, 1);

  // ft_RestoreContext();
  // ft_SetBase(10);
  // ft_Number(0, 0, 18, 0, filesize);

  ft_Display();
  // Формируем новый дисплей-лист, но показываем его только после загрузки
  // пикселей. Иначе FT812 рисует старые данные RAM_G под новым описателем.
  if (!fifo_flush() || !fifo_wait(0xFFC)) return;

  {
    u8 *bptr = fs_buf + sizeof(DXP);
    u16 bsz = min(filesize, FS_BUF_SIZE) - sizeof(DXP);

    if (d->type == 0)
    {
      u32 gptr = FT_RAM_G;

      while (sz)
      {
        u16 s = min(sz, bsz);
        ft_write(bptr, gptr, s);
        gptr += s, sz -= s, bptr = fs_buf, bsz = FS_BUF_SIZE;

        if (sz && !read_chunk(sz)) return;
      }
    }
    else
    {
      ft_ccmd_start(cmdl);
      ft_Inflate(FT_RAM_G);
      if (!fifo_flush()) return;

      while (sz)
      {
        u16 s = min(sz, bsz);
        if (!fifo_send(bptr, s)) return;
        sz -= s, bptr = fs_buf, bsz = FS_BUF_SIZE;

        if (sz && !read_chunk(sz)) return;
      }

      if (!fifo_wait(0xFFC)) return;
    }
  }
  ft_ccmd(FT_CCMD_SWAP);
  if (fifo_flush()) fifo_wait(0xFFC);
}

void show_dls()
{
  u32 left = filesize;
  u32 dest = FT_RAM_DL;
  // RAM_DL в FT812 — ровно 8 КиБ, инструкции имеют длину четыре байта.
  // Не допускаем запись за её границу или обращение к SDK с длиной ноль.
  if (!left || left > 8192 || (left & 3))
  { view_failed = 1; return; }
  wc_vmode(0x87);
  while (left)
  {
    u16 count = min(left, FS_BUF_SIZE);
    if (!read_chunk(left)) return;
    ft_write(fs_buf, dest, count);
    dest += count;
    left -= count;
  }
  // За коротким списком ставим DISPLAY: не исполняем хвост предыдущего DLS.
  if (filesize < 8192)
  {
    memset(fs_buf, 0, 4);
    ft_write(fs_buf, dest, 4);
  }
  ft_wreg8(FT_REG_DLSWAP, FT_DLSWAP_FRAME);
}

#else
#include "avi_controls.c"

// Штатное воспроизведение со звуком использует то же двухступенчатое чтение:
// упреждающий буфер в собственных страницах EVO -> media FIFO FT812.
// The list of the videos (Enter) is not built here (avi_list).
void show_avi()
{
  wc_video_clock(1);
  // REG_FREQUENCY is the clock of the mode, set by the I/O bank (it was
  // 64 MHz here for the one mode of the original).
  avi_first();
  do
  {
    u32 left = avi.size;
    avi_switch = 0;
    if (avi_controls()) continue;
    if (left < 12 || !read_chunk(left) ||
        memcmp(fs_buf, "RIFF", 4) || memcmp(fs_buf + 8, "AVI ", 4))
    { fail(17); return; }
    // Чужой кодек не отдаём CMD_PLAYVIDEO: он покажет мусор и упрётся в
    // таймаут. Плеер сообщает «Unsupported video codec». Разбор заголовка
    // для этой проверки не годится: он бросает файл и раньше — скажем,
    // когда размер в RIFF не сходится с длиной файла.
    if (!avi_mjpeg()) { view_failed = VIEW_CODEC; return; }

    wc_vmode(0x87);
    avi_restart(true);
  } while (avi_switch && !view_failed);
}
#endif


// --- Aux functions ------------------------
// Окно закрывает любая клавиша — теми же вызовами WC, что и его собственные
// окна: USPO ждёт, пока клавиши отпущены (иначе окно закрыла бы та, которой
// открыли просмотр), NUSP — нажатия любой.
void wait_any_key()
{
  wc_api__bool(_USPO);
  wc_api__bool(_NUSP);
}

// Окно ошибки или сведений до нажатия клавиши, затем выход в WC.
void show_window(const WC_TX_WINDOW *window)
{
  wc_api_u16(_PRWOW, (u16)window);
  wait_any_key();
  wc_api_u16(_RRESB, (u16)window);
  wc_exit(WC_EXIT);
}

void main_start()
{
  // ABT — накопленный флаг, не текущее состояние клавиши. WC не сбрасывает
  // его перед каждым плагином: старый Esc не должен отменять новый просмотр.
  *(volatile u8*)_ABT = 0;
  {
#ifndef FTVIEW_VIDEO_BANK
    if (!state_ft)
      state_ft = vdac2_init() ? 1 : 2;
#else
    state_ft = ft_state & 3;
    graphics_active = ft_state >> 7;
#endif

    if (state_ft != 1)
#ifndef FTVIEW_VIDEO_BANK
      show_window(&err_no_ft);
#else
      // The image bank shows the window: the video bank has no room.
      { list_error(AVI_MSG_NOFT); wc_exit(WC_EXIT); }
#endif

    else switch (file_ext)
    {
#ifndef FTVIEW_VIDEO_BANK
      case EXT_JPG:
      case EXT_PNG:
        show_jpg_png(file_ext == EXT_JPG);
      break;

      case EXT_DXP:
        show_dxp();
      break;

      case EXT_DLS:
        show_dls();
      break;
#else
      case EXT_AVI:
        show_avi();
      break;
#endif
    }
  }

  // Не оставляем WC ждать клавиш поверх незавершённой загрузки.
  if (view_failed)
  {
    view_cleanup();
    if (!*(volatile u8*)_ABT)
#ifndef FTVIEW_VIDEO_BANK
      show_window(view_failed == VIEW_TOO_LARGE ? &err_size : &err_file);
#else
      // The image bank shows the window and returns here.
      list_error(view_failed == VIEW_CODEC ? AVI_MSG_CODEC : AVI_MSG_FILE);
#endif
    wc_exit(WC_EXIT);
  }

  // Esc во время просмотра (ABT ставит прерывание WC, пока клавиша нажата)
  // — выход сразу. Цикл ниже смотрит, нажата ли Esc сейчас: короткое нажатие
  // во время перемотки или построения индекса к нему уже отпущено, и плагин
  // ждал второго Esc.
  if (*(volatile u8*)_ABT) wc_exit(WC_EXIT);

  // poll keys
  while (1)
  {
    __asm
      // HALT при DI не проснётся от клавиатуры. Включаем IM2 явно.
      ei
      halt
    __endasm;

    if (wc_api__bool(_PGD))   // PgDn - next file
      wc_exit(WC_NEXT_FILE);

    if (wc_api__bool(_PGU))   // PgUp - previous file
      wc_exit(WC_PREV_FILE);

    if (wc_api__bool(_ESC))   // Esc - exit
      wc_exit(WC_EXIT);

  }
}

// ------------------------------------------
// WC входит только в банк изображений; в видеобанке вход #8000 не нужен.
void main() __naked
{
#ifdef FTVIEW_VIDEO_BANK
  __asm
    ret
  __endasm;
#else
  __asm
    ld (_ret_sp), sp
    ld (_ret_ix), ix
    ld (_ret_iy), iy
    ld (_call_type), a
    ex af, af
    ld (_file_ext), a
    ld (_filesize), hl
    ld (_filesize + 2), de
    jp _main_dispatch
  __endasm;
#endif
}

// Входной регистровый контракт захвачен до возможного пролога SDCC.
void main_dispatch()
{
#ifndef FTVIEW_VIDEO_BANK
  // ABT — накопленный флаг Esc: WC не сбрасывает его перед плагином. Для AVI
  // gs_boot и vdac2_init идут здесь, ещё до main_start. Esc, которым вышли из
  // прошлого просмотра, обрывал ожидание FT812 после сброса, и плагин
  // сообщал «No VDAC2 detected!».
  *(volatile u8*)_ABT = 0;
  gs_loaded = 0;
  index_state = 0;
  // Переход выполняется до инициализации FT812. Шлюз восстанавливает входной
  // SP и не возвращается в старый банк: все пути выхода принадлежат видеобанку.
  // Драйвер GS загружается здесь: его код не занимает место видеобанка.
  if (call_type == WC_CALL_EXT && file_ext == EXT_AVI)
  {
    // The SD reader (sdr.c): its state goes to buffer page 1, the video
    // bank checks sdr.next (avi_first). Without it the stream of WC plays
    // the video, without the seek table.
    wc_map_buffer(1);
    IO_FAR->op = IO_OPEN;
    IO_FAR->size = filesize;
    far_call(IO_PAGE, IO_FAR);
    gs_loaded = gs_boot();
    graphics_active = 0;
    ft_state = (vdac2_init() ? 1 : 2) | gs_stage << 2;
    if (graphics_active) ft_state |= 0x80;
    if (scr_w == 640 && scr_h == 1024) ft_state |= 0x20;   // mode 10
    wc_video_bank();
  }
#endif
  view_failed = 0;
  state_ft = graphics_active = 0;
#ifndef FTVIEW_VIDEO_BANK
  switch (call_type)
  {
    case WC_CALL_EXT:
      wc_map_buffer(1);
      main_start();
    break;

    // Из меню F10 плагин больше не зовётся (тип +197 — только по
    // расширению); окно About с версией убрано вместе с этим вызовом.
    default:
      wc_exit(WC_EXIT);
  }
#else
  // Видеобанк получает управление только из шлюза для AVI.
  wc_map_buffer(1);
  main_start();
#endif
}


// Быстрая перемотка AVI. Код исполняется в банке изображений по вызову
// видеобанка (far_call): страница видео почти заполнена.
//
// Таблица строится при первой перемотке, а не при открытии файла. Запись k
// относится к кадру F = k*step: это первый чанк idx1, начиная с которого есть
// и кадр F, и звук с сэмпла S = F*pcm_rate/fps. Хранятся абсолютное смещение
// чанка, число видеочанков и звуковых байтов до него. Таблица лежит в RAM_G
// над двумя кадрами и не пересекается ни с media FIFO, ни с кольцом FT812.
//
// Перемотка подаёт FT812 заголовок AVI до 'movi', выравнивающий JUNK и поток
// с найденного чанка. Видеобанк досчитывает кадры до цели без показа и
// отбрасывает лишние сэмплы.

#define AVI_INDEX 0xD0000UL
#define AVI_INDEX_MAX 5460

#include "avi_shared.h"

// Общие поля. В видеобанке — рабочие переменные плеера, в банке изображений —
// копия для разбора заголовка. Имена полей — через макросы.
AVI_SHARED avi;
#define avi_total avi.total
#define avi_rate avi.rate
#define avi_scale avi.scale
#define avi_bytes avi.bytes
#define avi_write avi.write
#define avi_pcm_rate avi.pcm_rate
#define avi_pcm_total avi.pcm_total
#define avi_movi_start avi.movi_start
#define avi_movi_end avi.movi_end
#define avi_seek_pos avi.seek_pos
#define avi_seek_frame avi.seek_frame
#define avi_seek_audio avi.seek_audio
#define avi_period avi.period
#define avi_width avi.width
#define avi_height avi.height

// The start of the video: the SD reader at sector 0, or the stream of WC on
// the first sector of the file (GIPAGPL: the file WC started the plugin with;
// without the reader there is no other video to switch to).
bool avi_rewind()
{
  avi.disk = 0;
  return avi.io || wc_rewind();
}

#ifndef FTVIEW_VIDEO_BANK

// Состояние таблицы текущего файла: 0 — не строилась, 1 — есть, 2 — нет.
u8 index_state;
u16 index_step, index_count;

// Skip sectors of the file (after avi_rewind): the SD reader only moves its
// position. The stream of WC cannot skip without internals (LOADNON is
// reserved in the stock WC), so there is no seek table without the reader.
bool disk_skip(u32 sectors)
{
  if (!avi.io) { view_failed = 1; return false; }
  avi.disk += sectors;
  return true;
}

// Установить побайтовое чтение header_byte() на абсолютное смещение pos:
// начало файла, пропуск целых секторов, затем хвост сектора по байту.
u8 disk_moved;
bool file_seek(u32 pos)
{
  u16 skip = pos & 511;
  disk_moved = 1;
  if (!avi_rewind() || !disk_skip(pos >> 9)) return false;
  header_left = filesize - (pos & ~511UL);
  header_pos = header_size = 0;
  while (skip--) header_byte();
  return !view_failed;
}

// 16 байтов записи idx1: указатель в fs_buf или, на границе чтения, копия.
u8 idx_tmp[16];
u8 *idx_entry()
{
  u8 i;
  if (header_size - header_pos >= 16)
  {
    header_pos += 16;
    return fs_buf + header_pos - 16;
  }
  for (i = 0; i < 16; ++i) idx_tmp[i] = header_byte();
  return idx_tmp;
}

// Рабочие переменные построения — статические: стек ядра WC у плагина
// неглубокий, а здесь он глубже всего (перемотка -> чтение idx1 -> пропуск
// секторов в ядре). ib_exit — шаг, на котором построение остановилось
// (0 — таблица есть): после отказа его видно в памяти страницы.
// Раскладка — для ASM ниже: запись таблицы ib_entry — смещение чанка, затем
// бегущие счётчики видеочанков и звуковых байтов (их не нужно копировать в
// запись); смещение и длина чанка idx1 — рядом в ib_ol.
u32 ib_pos, ib_size, ib_n, ib_frame, ib_sample, ib_base, ib_tmp;
u32 ib_entry[3], ib_sample_step, ib_pcm_total, ib_ol[2];
u16 ib_k, ib_count, ib_step, ib_rem, ib_sample_rem, ib_fps;
u8 ib_tries, ib_exit, ib_kind;
#define ib_video ib_entry[1]
#define ib_audio ib_entry[2]

// Готовая таблица FTViewConvert — чанк JUNK между LIST movi и idx1 (другие
// плееры JUNK пропускают): подпись "FTVIEWI1", шаг и число записей (u16),
// побайтная копия полей AVI_SHARED от pcm_rate до pcm_total (30 байт: таблица
// сделана именно для этого файла), два байта выравнивания и записи в формате
// RAM_G. Записи поштучно проверяет index_seek, как и построенные по idx1.
// Вход — ib_n: FOURCC чанка (не JUNK — сразу 0), ib_size — его размер, чтение
// стоит на данных; 1 — таблица в RAM_G. Портит только ib_n и ib_ol
// (построение по idx1 задаёт их заново). Отказ снимает view_failed: чтение за
// концом оборванного файла не должно валить просмотр — тогда таблица строится
// по idx1 или её нет. На ASM: на C с 32-битными статическими переменными
// функция занимала 458 байт.
// Шаги: подпись (подпрограмма 00050$ сравнивает B байтов потока с (HL));
// шаг и число записей — в ib_n; 30 байт полей — с p+2 (IX = p, функции на C
// его сохраняют); два байта выравнивания; шаг не 0, записей 1..AVI_INDEX_MAX;
// ib_len = записи*12 (не больше 65520), чанк вмещает 44 + ib_len, файл не
// обрывается раньше (header_left + остаток буфера); затем записи порциями
// буфера чтения идут ft_write в RAM_G #D0000 + ib_off (SDK с ABI 0: n на
// время вызова — в ib_off + 2). Комментариев внутри __asm нет: SDCC
// переносит кириллицу в них как \U0000xxxx, и длинная строка роняет sdasz80.
const u8 ix_magic[8] = {'F', 'T', 'V', 'I', 'E', 'W', 'I', '1'};
bool index_embedded(AVI_SHARED *p) __naked
{
  p;
  __asm
    ld b, h
    ld c, l
    ld hl, (_ib_n)
    ld de, #0x554A
    or a
    sbc hl, de
    jr nz, 00091$
    ld hl, (_ib_n + 2)
    ld de, #0x4B4E
    sbc hl, de
    jr z, 00092$
00091$:
    xor a
    ret
00092$:
    push ix
    push bc
    pop ix
    ld hl, #_ix_magic
    ld b, #8
    call 00050$
    jp nz, 00090$
    ld hl, #_ib_n
    ld b, #4
00001$:
    push hl
    push bc
    call _header_byte
    pop bc
    pop hl
    ld (hl), a
    inc hl
    djnz 00001$
    push ix
    pop hl
    inc hl
    inc hl
    ld b, #30
    call 00050$
    jp nz, 00090$
    call _header_byte
    call _header_byte
    ld hl, (_ib_n)
    ld a, h
    or l
    jp z, 00090$
    ld hl, (_ib_n + 2)
    ld a, h
    or l
    jp z, 00090$
    ld de, #AVI_INDEX_MAX + 1
    sbc hl, de
    jp nc, 00090$
    ld hl, (_ib_n + 2)
    add hl, hl
    add hl, hl
    ld d, h
    ld e, l
    add hl, hl
    add hl, de
    ld (_ib_ol + 4), hl
    ld de, #44
    add hl, de
    ld a, #0
    adc a, a
    ex de, hl
    ld hl, (_ib_size)
    sbc hl, de
    ld e, a
    ld d, #0
    ld hl, (_ib_size + 2)
    sbc hl, de
    jp c, 00090$
    ld hl, (_header_size)
    ld de, (_header_pos)
    or a
    sbc hl, de
    ld de, (_header_left)
    add hl, de
    jr c, 00005$
    ld de, (_header_left + 2)
    ld a, d
    or e
    jr nz, 00005$
    ld de, (_ib_ol + 4)
    sbc hl, de
    jp c, 00090$
00005$:
    ld hl, #0
    ld (_ib_ol), hl
00010$:
    ld hl, (_ib_ol + 4)
    ld a, h
    or l
    jr z, 00020$
    ld hl, (_header_pos)
    ld de, (_header_size)
    or a
    sbc hl, de
    jr nz, 00011$
    call _header_byte
    ld hl, (_header_pos)
    dec hl
    ld (_header_pos), hl
00011$:
    ld a, (_view_failed)
    or a
    jp nz, 00090$
    ld hl, (_header_size)
    ld de, (_header_pos)
    sbc hl, de
    ld de, (_ib_ol + 4)
    push hl
    sbc hl, de
    pop hl
    jr c, 00012$
    ex de, hl
00012$:
    ld (_ib_ol + 2), hl
    push hl
    ld hl, #0x000D
    push hl
    ld hl, (_ib_ol)
    push hl
    ld hl, (_header_pos)
    ld de, #_fs_buf
    add hl, de
    push hl
    call _ft_write
    ld hl, #8
    add hl, sp
    ld sp, hl
    ld de, (_ib_ol + 2)
    ld hl, (_header_pos)
    add hl, de
    ld (_header_pos), hl
    ld hl, (_ib_ol)
    add hl, de
    ld (_ib_ol), hl
    ld hl, (_ib_ol + 4)
    or a
    sbc hl, de
    ld (_ib_ol + 4), hl
    jr 00010$
00020$:
    ld hl, (_ib_n + 2)
    ld (_index_count), hl
    ld hl, (_ib_n)
    ld (_index_step), hl
    ld a, #1
    ld (_index_state), a
    xor a
    ld (_ib_exit), a
    inc a
    pop ix
    ret
00090$:
    xor a
    ld (_view_failed), a
    pop ix
    ret
00050$:
    push hl
    push bc
    call _header_byte
    pop bc
    pop hl
    cp (hl)
    ret nz
    inc hl
    djnz 00050$
    ret
  __endasm;
}

// Проход idx1 (ib_n записей по 16 байт с текущей позиции чтения) — таблица
// до ib_count записей. Для каждого чанка: смещение и длина — в ib_ol, вид
// (ib_kind: 1 — видео "00dc"/"00db", 2 — звук "01wb", если он есть, 0 —
// прочее); пока ничего не сосчитано, база смещений: от FOURCC 'movi', если
// смещение меньше movi_start (так пишет ffmpeg), иначе абсолютные. Затем,
// пока запись k нужна этому чанку (видео с номером не меньше кадра k*step
// или звук, доходящий до сэмпла S(k)), — запись: base + смещение и бегущие
// счётчики (ib_entry) в RAM_G #D0000 + k*12; k, кадр и S(k) растут (S — с
// остатком от деления на fps, как в C-версии). Потом счётчик видеочанков +1
// или звуковых байтов + длина. Выход — по концу idx1, числу записей, ошибке
// чтения или Esc. Помощники: 00070$ — (HL)-1, 00075$ — (HL)+1, 00080$ —
// CF = (HL) < (DE), 00090$ — (DE) += (HL); все над u32 в памяти. IX = p.
// Было на C 1,3 КиБ; эталон — avigen.seek_table (IndexTests).
void index_scan(AVI_SHARED *p) __naked
{
  p;
  __asm
    push ix
    push hl
    pop ix
00001$:
    ld hl, #_ib_n
    ld a, (hl)
    inc hl
    or (hl)
    inc hl
    or (hl)
    inc hl
    or (hl)
    jp z, 00099$
    ld hl, #_ib_n
    call 00070$
    ld hl, (_ib_k)
    ld de, (_ib_count)
    or a
    sbc hl, de
    jp nc, 00099$
    call _idx_entry
    ld a, (_view_failed)
    ld hl, #0x6004
    or (hl)
    jp nz, 00099$
    push de
    ld hl, #8
    add hl, de
    ld de, #_ib_ol
    ld bc, #8
    ldir
    pop hl
    ld c, #0
    ld a, (hl)
    cp #0x30
    jr nz, 00010$
    inc hl
    ld a, (hl)
    inc hl
    cp #0x31
    jr z, 00004$
    cp #0x30
    jr nz, 00010$
    ld a, (hl)
    cp #0x64
    jr nz, 00010$
    inc hl
    ld a, (hl)
    cp #0x63
    jr z, 00003$
    cp #0x62
    jr nz, 00010$
00003$:
    inc c
    jr 00010$
00004$:
    ld a, (hl)
    cp #0x77
    jr nz, 00010$
    inc hl
    ld a, (hl)
    cp #0x62
    jr nz, 00010$
    ld hl, #_ib_pcm_total
    ld a, (hl)
    inc hl
    or (hl)
    inc hl
    or (hl)
    inc hl
    or (hl)
    jr z, 00010$
    ld c, #2
00010$:
    ld a, c
    ld (_ib_kind), a
    ld hl, (_ib_k)
    ld a, h
    or l
    jr nz, 00020$
    ld hl, #_ib_entry + 4
    ld b, #8
00011$:
    or (hl)
    inc hl
    djnz 00011$
    jr nz, 00020$
    push ix
    pop de
    ld hl, #4
    add hl, de
    push hl
    ld de, #_ib_base
    ld bc, #4
    ldir
    pop de
    ld hl, #_ib_ol
    call 00080$
    ld hl, #_ib_base
    jr c, 00012$
    xor a
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl
    ld (hl), a
    inc hl
    ld (hl), a
    jr 00020$
00012$:
    ld a, (hl)
    sub #4
    ld (hl), a
    ld b, #3
00013$:
    inc hl
    ld a, (hl)
    sbc a, #0
    ld (hl), a
    djnz 00013$
00020$:
    ld hl, (_ib_k)
    ld de, (_ib_count)
    or a
    sbc hl, de
    jp nc, 00040$
    ld a, (_ib_kind)
    dec a
    jr nz, 00022$
    ld hl, #_ib_entry + 4
    ld de, #_ib_frame
    call 00080$
    jp c, 00040$
    jr 00030$
00022$:
    dec a
    jp nz, 00040$
    ld hl, #_ib_entry + 8
    ld de, #_ib_tmp
    ld bc, #4
    ldir
    ld hl, #_ib_ol + 4
    ld de, #_ib_tmp
    call 00090$
    ld hl, #_ib_sample
    ld de, #_ib_tmp
    call 00080$
    jp nc, 00040$
00030$:
    ld hl, #_ib_base
    ld de, #_ib_entry
    ld bc, #4
    ldir
    ld hl, #_ib_ol
    ld de, #_ib_entry
    call 00090$
    ld hl, #12
    push hl
    ld hl, #0x000D
    push hl
    ld hl, (_ib_k)
    add hl, hl
    add hl, hl
    ld d, h
    ld e, l
    add hl, hl
    add hl, de
    push hl
    ld hl, #_ib_entry
    push hl
    call _ft_write
    pop af
    pop af
    pop af
    pop af
    ld hl, (_ib_k)
    inc hl
    ld (_ib_k), hl
    ld hl, (_ib_frame)
    ld de, (_ib_step)
    add hl, de
    ld (_ib_frame), hl
    jr nc, 00031$
    ld hl, (_ib_frame + 2)
    inc hl
    ld (_ib_frame + 2), hl
00031$:
    ld hl, #_ib_sample_step
    ld de, #_ib_sample
    call 00090$
    ld hl, (_ib_rem)
    ld de, (_ib_sample_rem)
    add hl, de
    ld de, (_ib_fps)
    or a
    sbc hl, de
    jr nc, 00032$
    add hl, de
    ld (_ib_rem), hl
    jp 00020$
00032$:
    ld (_ib_rem), hl
    ld hl, #_ib_sample
    call 00075$
    jp 00020$
00040$:
    ld a, (_ib_kind)
    dec a
    jr nz, 00041$
    ld hl, #_ib_entry + 4
    call 00075$
    jp 00001$
00041$:
    dec a
    jp nz, 00001$
    ld hl, #_ib_ol + 4
    ld de, #_ib_entry + 8
    call 00090$
    jp 00001$
00099$:
    pop ix
    ret
00070$:
    ld b, #4
    scf
00071$:
    ld a, (hl)
    sbc a, #0
    ld (hl), a
    inc hl
    ret nc
    djnz 00071$
    ret
00075$:
    ld b, #4
00076$:
    inc (hl)
    ret nz
    inc hl
    djnz 00076$
    ret
00080$:
    ld b, #4
    or a
00081$:
    ld a, (de)
    ld c, a
    ld a, (hl)
    sbc a, c
    inc hl
    inc de
    djnz 00081$
    ret
00090$:
    ld b, #4
    or a
00091$:
    ld a, (de)
    adc a, (hl)
    ld (de), a
    inc hl
    inc de
    djnz 00091$
    ret
  __endasm;
}

void index_build(AVI_SHARED *p)
{
  ib_frame = ib_sample = ib_video = ib_audio = ib_base = ib_sample_step = 0;
  ib_k = ib_rem = ib_sample_rem = 0;
  ib_fps = p->rate;
  index_state = 2;
  ib_exit = 1;
  // The table and the seek need the SD reader (disk_skip).
  if (!avi.io) return;
  // Кадры над таблицей; заголовок до 'movi' повторно подаётся в FIFO и
  // ограничен 64 КиБ; для звука нужна целая частота кадров.
  if (p->bytes > AVI_INDEX / 2 || p->movi_start > 0x10000UL ||
      (p->pcm_rate && (p->scale != 1 || !ib_fps))) return;
  ib_pcm_total = p->pcm_rate ? p->pcm_total : 0;
  // idx1 обычно сразу за LIST movi; допускаем несколько чанков JUNK. Чанк JUNK
  // с готовой таблицей FTViewConvert стоит перед idx1: тогда idx1 не нужен.
  ib_pos = p->movi_end + (p->movi_end & 1);
  ib_exit = 2;
  for (ib_tries = 0;; ++ib_tries)
  {
    if (ib_tries == 8 || ib_pos + 8 > filesize || !file_seek(ib_pos)) return;
    ib_n = avi_word();
    ib_size = avi_word();
    if (ib_n == 0x31786469UL) break; // idx1
    if (index_embedded(p)) return;   // JUNK с готовой таблицей
    ib_pos += 8 + ib_size + (ib_size & 1);
  }
  ib_exit = 3;
  if (ib_size > filesize - ib_pos - 8) return;
  ib_exit = 4;
  ib_n = ib_size >> 4;
  ib_step = p->total / AVI_INDEX_MAX + 1;
  ib_count = (p->total + ib_step - 1) / ib_step;
  if (ib_pcm_total)
  {
    // S(k+1) = S(k) + step*pcm_rate/fps с остатком: без деления на каждую
    // запись и с тем же округлением вниз, что pcm_sample() видеобанка.
    ib_sample_step = (u32)ib_step * p->pcm_rate;
    ib_sample_rem = ib_sample_step % ib_fps;
    ib_sample_step /= ib_fps;
  }
  index_scan(p);
  if (view_failed || *(volatile u8*)_ABT) return;
  // Кадры за обрезанным idx1 перематываются от последней записи.
  ib_exit = 5;
  if (!ib_k) return;
  index_count = ib_k;
  index_step = ib_step;
  index_state = 1;
  ib_exit = 0;
}

// Точка входа для p->target. Испорченная запись не должна увести поток за
// пределы movi; тогда, как и без таблицы, возвращаются нули.
u32 is_e[3];
void index_seek(AVI_SHARED *p)
{
  u32 *e = is_e;
  u16 k;
  p->seek_pos = p->seek_frame = p->seek_audio = 0;
  // Переходу в начало таблица не нужна, а её построение проходит весь файл.
  if (!p->target) return;
  disk_moved = 0;
  if (!index_state) index_build(p);
  if (disk_moved) p->ok = 2;
  if (index_state != 1) return;
  k = p->target / index_step;
  if (k >= index_count) k = index_count - 1;
  ft_read(e, AVI_INDEX + (u32)k * 12, 12);
  if (e[0] > p->movi_start && e[0] < p->movi_end && !(e[0] & 1) && e[1] <= p->target)
  {
    p->seek_pos = e[0];
    p->seek_frame = e[1];
    p->seek_audio = e[2];
  }
}

// Заголовок AVI до FOURCC 'movi' включительно и выравнивающий чанк JUNK.
// За ними в FIFO идёт файл с base = seek_pos & ~3, так что CMD_VIDEOSTART и
// CMD_VIDEOFRAME видят непрерывный поток чанков; байты base..seek_pos
// становятся хвостом JUNK. DMA передаёт длины, кратные четырём: смещения
// чанков AVI и сам заголовок чётны, поэтому заполнитель — 0 или 2 байта.
// На выходе диск стоит на секторе base: видеобанк читает дальше сам.
bool index_header(AVI_SHARED *p)
{
  u32 left = p->movi_start;
  u16 count;
  wc_map_buffer(1);
  if (!avi_rewind()) return false;
  while (left)
  {
    count = min(left, FS_BUF_SIZE);
    if (!read_chunk(left)) return false;
    left -= count;
    if (!left)
    {
      u8 fill = -(u8)(p->movi_start + 8) & 3;
      memcpy(fs_buf + count, "JUNK\0\0\0\0\0\0", 10);
      fs_buf[count + 4] = fill + (p->seek_pos & 3);
      count += 8 + fill;
    }
    ft_load_ram_dma(fs_buf, *(u8*)_PAGE3, 0xE0000UL + p->write, count);
    p->write += count;
  }
  ft_wreg32(REG_MEDIAFIFO_WRITE, p->write);
  // Пропуск идёт от начала файла: проход цепочки начинается с границы
  // кластера, а заголовок мог закончиться посреди кластера.
  return avi_rewind() && disk_skip(p->seek_pos >> 9);
}

u32 avi_position()
{
  return filesize - header_left - header_size + header_pos;
}

// Обходим реальные границы LIST/чанков. Не ищем строки avih/strh внутри
// данных изображения или JUNK. Глубина вложения ограничена рабочим стеком.
// Разбор выполняется один раз на файл, поэтому живёт в банке изображений.
// The audio strh into page 1 (AUD_STRH: scale, rate, length, sample size)
// for avi_sound of the I/O bank.
#define a_scale AUD_STRH[0]
#define a_rate AUD_STRH[1]
#define a_len AUD_STRH[2]
#define a_ss AUD_STRH[3]
bool avi_probe()
{
  u32 ends[4], pos, id, size, end;
  u8 depth = 0, video = 0, sound = 0, stream = 0, kind = 0, pcm_ok = 0, esp_ok = 0;
  header_left = filesize; header_pos = header_size = 0;
  avi_period = avi_total = 0;
  avi_scale = avi_rate = 0;
  avi_width = avi_height = 0;
  avi_pcm_rate = 0; avi_pcm_total = 0;
  if (avi_word() != 0x46464952UL) return false; // RIFF
  size = avi_word();
  if (filesize < 12 || size < 4 || size > filesize - 8 ||
      avi_word() != 0x20495641UL) return false; // AVI
  ends[0] = size + 8;
  for (;;)
  {
    pos = avi_position();
    while (pos == ends[depth] && depth) --depth;
    if (view_failed || pos > ends[depth] || ends[depth] - pos < 8) return false;
    id = avi_word(); size = avi_word(); pos += 8;
    if (size > ends[depth] - pos) return false;
    end = pos + size;
    if (id == 0x5453494CUL) // LIST
    {
      if (size < 4) return false;
      id = avi_word();
      if (id == 0x69766F6DUL)
      { avi_movi_start = avi_position(); avi_movi_end = end; break; } // movi
      if (id == 0x6C726468UL || id == 0x6C727473UL) // hdrl / strl
      {
        if (id == 0x6C727473UL) { ++stream; kind = 0; }
        if (depth == 3 || (end & 1)) return false;
        ends[++depth] = end;
        continue;
      }
    }
    else if (id == 0x68697661UL && size >= 40) // avih
    {
      u8 field;
      for (field = 0; field < 10; ++field)
      {
        u32 value = avi_word();
        if (field == 0) avi_period = value;
        if (field == 4) avi_total = value;
        if (field == 8) { if (value > 2047) return false; avi_width = value; }
        if (field == 9) { if (value > 2047) return false; avi_height = value; }
      }
    }
    else if (id == 0x68727473UL && size >= 28) // strh
    {
      id = avi_word();
      // Регистр FOURCC не важен: mjpg — тот же MJPEG. Чужой кодек ловит
      // видеобанк перед штатным воспроизведением (avi_mjpeg): разбор сюда
      // доходит не всегда (например, у файла с обрезанным хвостом).
      if (id == 0x73646976UL && (avi_word() | 0x20202020UL) == 0x67706A6DUL) // vids/MJPG
      {
        if (stream != 1) return false;
        video = 1;
        avi_word(); avi_word(); avi_word();
        avi_scale = avi_word(); avi_rate = avi_word();
      }
      if (id == 0x73647561UL) // auds
      {
        sound = 1; kind = 1;
        if (stream != 2 || size < 48) return false;
        avi_word(); avi_word(); avi_word(); avi_word(); // handler/flags/priority/initial
        a_scale = avi_word(); a_rate = avi_word();
        if (avi_word()) return false; // общая нулевая временная шкала
        a_len = avi_word();
        avi_word(); avi_word();       // buffer size, quality
        a_ss = avi_word();
      }
    }
    else if (id == 0x66727473UL && kind && size >= 16) // audio strf/WAVEFORMAT
    {
      // WAVEFORMATEX (with cbSize and IMA's wSamplesPerBlock) into page 1
      // for the WAV header of the ESP stream (esp.c). The ESP takes PCM 8 /
      // 16, IMA ADPCM, A-law, u-law; GS and FT812 only PCM 8 bit mono
      // (unsigned, a byte a sample). Other sound stays with CMD_PLAYVIDEO.
      u8 k;
      u16 *w = (u16*)ESP_WFX;
      for (k = 0; k < 5; ++k) ((u32*)w)[k] = k < 4 || size >= 20 ? avi_word() : 0;
      pcm_ok = w[0] == 1 && w[1] == 1 && w[7] == 8 && w[2] == a_rate && !w[3] && a_scale == 1;
      esp_ok = (w[0] == 1 || w[0] == 0x11 || w[0] == 6 || w[0] == 7 || w[0] == 0x55) && w[1] <= 2;
    }
    end += size & 1;
    if (end > ends[depth]) return false;
    while (avi_position() < end && !view_failed) header_byte();
  }
  avi_bytes = ((u32)avi_width * avi_height * 2 + 3) & 0xFFFFFFFCUL;
  // The sound (sink, rate, total ...) is chosen by avi_sound of the I/O
  // bank (no room here): the flags of the format for it.
  *AUD_FLAGS = sound | pcm_ok << 1 | esp_ok << 2 | gs_loaded << 3 | (ft_state & 0x40) >> 2;
  return !view_failed && video && avi_width && avi_height &&
         avi_total && avi_period >= 1000 && avi_period <= 10000000UL &&
         avi_bytes <= AVI_RESULT / 2;
}

// The codec by the start of the file in fs_buf (from the list bank of WC
// Improved): 1 — the first strh is MJPG video (any case of the FOURCC), 0 —
// another codec, 2 — nothing to judge by (not RIFF/AVI, no strh).
u8 buf_codec()
{
  u8 *q;
  if (memcmp(fs_buf, "RIFF", 4) || memcmp(fs_buf + 8, "AVI ", 4)) return 2;
  for (q = fs_buf + 12; q < fs_buf + 496; ++q)
    if (q[0] == 's' && q[1] == 't' && q[2] == 'r' && q[3] == 'h')
      return !memcmp(q + 8, "vids", 4) && (q[12] | 32) == 'm' && (q[13] | 32) == 'j' &&
             (q[14] | 32) == 'p' && (q[15] | 32) == 'g';
  return 2;
}

void far_entry(AVI_SHARED *p)
{
  view_failed = 0;
  filesize = p->size;
  avi.io = p->io;
  avi.disk = p->disk;
  if (p->op == AVI_OP_MJPEG) p->ok = buf_codec() != 0;
  else if (p->op == AVI_OP_MSG) avi_message(p->msg, p->code);
  else if (p->op == AVI_OP_SEEK) index_seek(p);
  else if (p->op == AVI_OP_HEADER) p->ok = index_header(p) && !view_failed;
  else if (p->op == AVI_OP_PROBE)
  {
    // Разбор — первое, что делается с файлом: таблица перемотки прежнего
    // ролика недействительна.
    index_state = 0;
    avi = *p;
    avi.ok = avi_probe();
    if (view_failed) avi.ok = 2;
    *p = avi;
    // The frame on the screen: the I/O bank (avi_view), room there.
    // The sound and the frame on the screen: the I/O bank (avi_sound,
    // avi_view), no room here.
    if (avi.ok == 1) { IO_FAR->op = IO_AVI; far_call(IO_PAGE, IO_FAR); }
  }
  p->disk = avi.disk;
}

#else

void far_entry(AVI_SHARED *p)
{
  p;
}

#endif

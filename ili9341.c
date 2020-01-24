/*
 * Copyright (c) 2014-2015, TAKAHASHI Tomohiro (TTRFTECH) edy555@gmail.com
 * All rights reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * The software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */
#include "ch.h"
#include "hal.h"
#include "nanovna.h"
#include "stm32_dma.h"

#define RESET_ASSERT	        palClearLine(LINE_LCD_RESET)
#define RESET_NEGATE	        palSetLine(LINE_LCD_RESET)
#define CS_LOW			palClearLine(LINE_LCD_CS)
#define CS_HIGH			palSetLine(LINE_LCD_CS)
#define DC_CMD			palClearLine(LINE_LCD_CD)
#define DC_DATA			palSetLine(LINE_LCD_CD)

uint16_t spi_buffer[SPI_BUFFER_SIZE];

//static void ssp_wait(void)
//{
//  while (SPI1->SR & SPI_SR_BSY)
//    ;
//}

static void ssp_wait_slot(void)
{
  while ((SPI1->SR & 0x1800) == 0x1800)
    ;
}

static void ssp_senddata(uint8_t x)
{
  *(uint8_t*)(&SPI1->DR) = x;
  while (SPI1->SR & SPI_SR_BSY)
    ;
}

static uint8_t ssp_sendrecvdata(uint8_t x)
{
    while (!(SPI1->SR & SPI_SR_TXE));
    // clear OVR
    while (SPI1->SR & SPI_SR_RXNE) (void)SPI1->DR;

    *(uint8_t*)(&SPI1->DR) = x;
    while (!(SPI1->SR & SPI_SR_RXNE));
    return SPI1->DR;
}

static void ssp_senddata16(uint16_t x)
{
  ssp_wait_slot();
  SPI1->DR = x;
  //while (SPI1->SR & SPI_SR_BSY)
  //  ;
}

 #ifdef ILI9488
static inline void ssp_senddata16RGB(uint16_t x)
{
  uint8_t r = (x & 0xF800) >> 8;
  uint8_t g = (x & 0x07E0) >> 3;
  uint8_t b = (x & 0x001F) << 3;
  ssp_senddata(r);
  ssp_senddata(g);
  ssp_senddata(b);
}
#endif

static void ssp_databit8(void)
{
  SPI1->CR2 = (SPI1->CR2 & 0xf0ff) | 0x0700;
//LPC_SSP1->CR0 = (LPC_SSP1->CR0 & 0xf0) | SSP_DATABIT_8;
}

//static void ssp_databit16(void)
//{
//  SPI1->CR2 = (SPI1->CR2 & 0xf0ff) | 0x0f00;
//  //LPC_SSP1->CR0 = (LPC_SSP1->CR0 & 0xf0) | SSP_DATABIT_16;
//}


static const stm32_dma_stream_t  *dmatx;
static uint32_t txdmamode;

static void spi_lld_serve_tx_interrupt(SPIDriver *spip, uint32_t flags) {
  (void)spip;
  (void)flags;
}

static void spi_init(void)
{
  rccEnableSPI1(FALSE);

  dmatx     = STM32_DMA_STREAM(STM32_SPI_SPI1_TX_DMA_STREAM);
  txdmamode = STM32_DMA_CR_CHSEL(SPI1_TX_DMA_CHANNEL) |
    STM32_DMA_CR_PL(STM32_SPI_SPI1_DMA_PRIORITY) |
    STM32_DMA_CR_DIR_M2P |
    STM32_DMA_CR_DMEIE |
    STM32_DMA_CR_TEIE |
    STM32_DMA_CR_PSIZE_HWORD |
    STM32_DMA_CR_MSIZE_HWORD;
  dmaStreamAllocate(dmatx,
                    STM32_SPI_SPI1_IRQ_PRIORITY,
                    (stm32_dmaisr_t)spi_lld_serve_tx_interrupt,
                    NULL);
  dmaStreamSetPeripheral(dmatx, &SPI1->DR);

  SPI1->CR1 = 0;
  SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI;// | SPI_CR1_BR_1;
  SPI1->CR2 = 0x0700 | SPI_CR2_TXDMAEN | SPI_CR2_FRXTH;
  SPI1->CR1 |= SPI_CR1_SPE;  
}

static void send_command(uint8_t cmd, int len, const uint8_t *data)
{
  CS_LOW;
  DC_CMD;
  ssp_databit8();
  ssp_senddata(cmd);
  DC_DATA;
  #ifdef ILI9488
  if ((cmd==0x2C) && (len==2)) {
    // write pixel, convert to RGB888
      int color = (data[1]<<8)+data[0];
      uint8_t r = (color & 0xF800) >> 8;
      uint8_t g = (color & 0x07E0) >> 3;
      uint8_t b = (color & 0x001F)<<3;
      ssp_senddata(r);
      ssp_senddata(g);
      ssp_senddata(b);
  } else {
    while (len-- > 0) {
      ssp_senddata(*data++);
    }
  }
  #else
  while (len-- > 0) {
    ssp_senddata(*data++);
  }
  #endif
  //CS_HIGH;
}

//static void send_command16(uint8_t cmd, int data)
//{
//	CS_LOW;
//	DC_CMD;
//    ssp_databit8();
//	ssp_senddata(cmd);
//	DC_DATA;
//    ssp_databit16();
//	ssp_senddata16(data);
//	CS_HIGH;
//}

static const uint8_t ili9341_init_seq[] = {
		// cmd, len, data...,
		// Power control B
		0xCF, 3, 0x00, 0x83, 0x30,
		// Power on sequence control
		0xED, 4, 0x64, 0x03, 0x12, 0x81,
		//0xED, 4, 0x55, 0x01, 0x23, 0x01,
		// Driver timing control A
		0xE8, 3, 0x85, 0x01, 0x79,
		//0xE8, 3, 0x84, 0x11, 0x7a,
		// Power control A
		0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,
		// Pump ratio control
		0xF7, 1, 0x20,
		// Driver timing control B
		0xEA, 2, 0x00, 0x00,
		// POWER_CONTROL_1
		0xC0, 1, 0x26,
		// POWER_CONTROL_2
		0xC1, 1, 0x11,
		// VCOM_CONTROL_1
		0xC5, 2, 0x35, 0x3E,
		// VCOM_CONTROL_2
		0xC7, 1, 0xBE,
		// MEMORY_ACCESS_CONTROL
		//0x36, 1, 0x48, // portlait
		0x36, 1, 0x28, // landscape, BGR565
		// COLMOD_PIXEL_FORMAT_SET : 16 bit pixel
		0x3A, 1, 0x55,
		// Frame Rate
		0xB1, 2, 0x00, 0x1B,
		// Gamma Function Disable
		0xF2, 1, 0x08,
		// gamma set for curve 01/2/04/08
		0x26, 1, 0x01,
		// positive gamma correction
		0xE0, 15, 0x1F,  0x1A,  0x18,  0x0A,  0x0F,  0x06,  0x45,  0x87,  0x32,  0x0A,  0x07,  0x02,  0x07, 0x05,  0x00,
		// negativ gamma correction
		0xE1, 15, 0x00,  0x25,  0x27,  0x05,  0x10,  0x09,  0x3A,  0x78,  0x4D,  0x05,  0x18,  0x0D,  0x38, 0x3A,  0x1F,

		// Column Address Set
	    0x2A, 4, 0x00, 0x00, 0x01, 0x3f, // width 320
	    // Page Address Set
	    0x2B, 4, 0x00, 0x00, 0x00, 0xef, // height 240

		// entry mode
		0xB7, 1, 0x06,
		// display function control
		0xB6, 4, 0x0A, 0x82, 0x27, 0x00,

		// control display
		//0x53, 1, 0x0c,
		// diaplay brightness
		//0x51, 1, 0xff,

		// sleep out
		0x11, 0,
		0 // sentinel
};

const uint8_t ili9488_init_seq[] = {
  //Interface Mode Control
  0xB0,1,0x00,
  //Frame Rate
  0xB1, 1, 0xA,
  //Display Inversion Control , 2 Dot
  0xB4, 1, 0x02,
  //RGB/MCU Interface Control
  0xB6, 3, 0x02, 0x02, 0x3B, 
  //EntryMode
  0xB7, 1, 0xC6,
  //Power Control 1
  0xC0, 2, 0x17, 0x15,
  //Power Control 2
  0xC1, 1, 0x41,
  //Power Control 3
    // 0xC5, 3, 0x00, 0x4D, 0x90,
  //VCOM Control
  0xC5, 3, 0x00, 0x12, 0x80,
  //Memory Access	
  // ILI9488_MADCTL, 1, MADCTL_MX | MADCTL_BGR
  0x36, 1, 0x28,  // landscape, BGR
  //0x36, 1, 0x20,  // landscape, RGB
  //16bpp DPI and DBI and
  //Interface Pixel Format	
  #if defined(ILI9488)
  0x3A, 1, 0x66,
  #else
  0x3A, 1, 0x55,
  #endif
  //default gamma	
    // 0xC0, 2, 0x18, 0x16,
    // 0xBE, 2, 0x00, 0x04,
  //P-Gamma
  0xE0, 15, 0x00, 0x03, 0x09, 0x08,
            0x16, 0x0A, 0x3F, 0x78,
            0x4C, 0x09, 0x0A, 0x08,
            0x16, 0x1A, 0x0F,
  //N-Gamma
  0xE1, 15, 0x00, 0X16, 0X19, 0x03,
            0x0F, 0x05, 0x32, 0x45,
            0x46, 0x04, 0x0E, 0x0D,
            0x35, 0x37, 0x0F,
  //Set Image Func
  0xE9, 1, 0x00, 
  //Set Brightness to Max
  0x51, 1, 0xFF,
  //Adjust Control
  0xF7, 4, 0xA9, 0x51, 0x2C, 0x82,
  //set default rotation to 0
  //Exit Sleep
  0x11, 0x00,
  // sentinel
  0
};



void ili9341_init(void)
{
    chMtxLock(&mutex_ili9341);
    spi_init();

  DC_DATA;
  RESET_ASSERT;
  chThdSleepMilliseconds(10);
  RESET_NEGATE;

  send_command(0x01, 0, NULL); // SW reset
  chThdSleepMilliseconds(5);
  send_command(0x28, 0, NULL); // display off

  const uint8_t *p;
  #if defined(ILI9488) || defined(ILI9486) || defined(ST7796S)
  for (p = ili9488_init_seq; *p; ) {
  #else
  for (p = ili9341_init_seq; *p; ) {
  #endif
    send_command(p[0], p[1], &p[2]);
    p += 2 + p[1];
    chThdSleepMilliseconds(5);
  }

  chThdSleepMilliseconds(100);
  send_command(0x29, 0, NULL); // display on
  chMtxUnlock(&mutex_ili9341);
}

void ili9341_pixel(int x, int y, int color)
{
	uint8_t xx[4] = { x >> 8, x, (x+1) >> 8, (x+1) };
	uint8_t yy[4] = { y >> 8, y, (y+1) >> 8, (y+1) };
	uint8_t cc[2] = { color >> 8, color };
	send_command(0x2A, 4, xx);
    send_command(0x2B, 4, yy);
    send_command(0x2C, 2, cc);
    //send_command16(0x2C, color);
}



#if defined(ILI9488) 
void ili9341_fill(int x, int y, int w, int h, int color)
{
  chMtxLock(&mutex_ili9341);
  if (((x+w)>LCD_WIDTH) || ((y+h)>LCD_HEIGHT)) {
    return;
  }
  uint8_t xx[4] = { x >> 8, x, (x+w-1) >> 8, (x+w-1) };
  uint8_t yy[4] = { y >> 8, y, (y+h-1) >> 8, (y+h-1) };
  int len = w * h;
  uint8_t r = (color & 0xF800) >> 8;
  uint8_t g = (color & 0x07E0) >> 3;
  uint8_t b = (color & 0x001F) << 3;
  uint16_t rg = r<<8 | g;
  uint16_t br = b<<8 | r;
  uint16_t gb = g<<8 | b;  
  send_command(0x2A, 4, xx);
  send_command(0x2B, 4, yy);
  send_command(0x2C, 0, NULL);
  while (len > 0) {
    ssp_senddata16(rg);
    ssp_senddata16(br);
    ssp_senddata16(gb);
    len -= 2;
  }
  chMtxUnlock(&mutex_ili9341);
}
#else
void ili9341_fill(int x, int y, int w, int h, int color)
{
	chMtxLock(&mutex_ili9341);
	uint8_t xx[4] = { x >> 8, x, (x+w-1) >> 8, (x+w-1) };
	uint8_t yy[4] = { y >> 8, y, (y+h-1) >> 8, (y+h-1) };
    int len = w * h;
	send_command(0x2A, 4, xx);
    send_command(0x2B, 4, yy);
    send_command(0x2C, 0, NULL);
    while (len-- > 0)
      ssp_senddata16(color);
	chMtxUnlock(&mutex_ili9341);
}
#endif

#if 0
void ili9341_bulk(int x, int y, int w, int h)
{
    chMtxLock(&mutex_ili9341);
	uint8_t xx[4] = { x >> 8, x, (x+w-1) >> 8, (x+w-1) };
	uint8_t yy[4] = { y >> 8, y, (y+h-1) >> 8, (y+h-1) };
	uint16_t *buf = spi_buffer;
    int len = w * h;
	send_command(0x2A, 4, xx);
	send_command(0x2B, 4, yy);
	send_command(0x2C, 0, NULL);
    while (len-- > 0) 
      ssp_senddata16(*buf++);
    chMtxUnlock(&mutex_ili9341);
}
#else
void ili9341_bulk(int x, int y, int w, int h)
{
    chMtxLock(&mutex_ili9341);
	uint8_t xx[4] = { x >> 8, x, (x+w-1) >> 8, (x+w-1) };
	uint8_t yy[4] = { y >> 8, y, (y+h-1) >> 8, (y+h-1) };
    int len = w * h;

	send_command(0x2A, 4, xx);
	send_command(0x2B, 4, yy);
	send_command(0x2C, 0, NULL);
    #ifdef ILI9488
    uint16_t *buf=spi_buffer;
    ssp_databit8();
    while (len-- > 0) 
      ssp_senddata16RGB(*buf++);
    ssp_databit16();
    #else
    dmaStreamSetMemory0(dmatx, spi_buffer);
    dmaStreamSetTransactionSize(dmatx, len);
    dmaStreamSetMode(dmatx, txdmamode | STM32_DMA_CR_MINC);
    dmaStreamEnable(dmatx);
    dmaWaitCompletion(dmatx);
    #endif
	chMtxUnlock(&mutex_ili9341);
}
#endif

static void ili9341_read_memory_raw(uint8_t cmd, int len, uint16_t* out)
{
    uint8_t r, g, b;
    send_command(cmd, 0, NULL);
    ssp_databit8();

    // consume old data
    while (!(SPI1->SR & SPI_SR_TXE));
    // clear OVR
    while (SPI1->SR & SPI_SR_RXNE) r = SPI1->DR;

    // require 8bit dummy clock
    r = ssp_sendrecvdata(0);

    while (--len > 0) {



	#ifdef ST7796S  // read data is 16bit
        r = ssp_sendrecvdata(0);
        g = ssp_sendrecvdata(0);
        *out++ = (r << 8) | g ;
	#else // read data is always 18bit
        r = ssp_sendrecvdata(0);
        g = ssp_sendrecvdata(0);
        b = ssp_sendrecvdata(0);
        *out++ = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
	#endif
    }

    CS_HIGH;
}

void ili9341_read_memory(int x, int y, int w, int h, int len, uint16_t *out)
{
    chMtxLock(&mutex_ili9341);
    uint8_t xx[4] = { x >> 8, x, (x+w) >> 8, (x+w) };
    uint8_t yy[4] = { y >> 8, y, (y+h) >> 8, (y+h) };

    send_command(0x2A, 4, xx);
    send_command(0x2B, 4, yy);

    ili9341_read_memory_raw(0x2E, len, out);
    chMtxUnlock(&mutex_ili9341);
}

void ili9341_read_memory_continue(int len, uint16_t* out)
{
    chMtxLock(&mutex_ili9341);
    ili9341_read_memory_raw(0x3E, len, out);
    chMtxUnlock(&mutex_ili9341);
}

#if !defined(ST7796S)
void ili9341_drawchar_5x7(uint8_t ch, int x, int y, uint16_t fg, uint16_t bg)
{
    chMtxLock(&mutex_ili9341);
    uint16_t *buf = spi_buffer;
    uint8_t bits;
    int c, r;
    for(c = 0; c < 7; c++) {
        bits = x5x7_bits[(ch * 7) + c];
        for (r = 0; r < 5; r++) {
            *buf++ = (0x80 & bits) ? fg : bg;
            bits <<= 1;
        }
    }
    ili9341_bulk(x, y, 5, 7);
    chMtxUnlock(&mutex_ili9341);
}

void ili9341_drawstring_5x7(const char *str, int x, int y, uint16_t fg, uint16_t bg)
{
    chMtxLock(&mutex_ili9341);
    while (*str) {
        ili9341_drawchar_5x7(*str, x, y, fg, bg);
        x += 5;
        str++;
    }
    chMtxUnlock(&mutex_ili9341);
}

void ili9341_drawchar_size(uint8_t ch, int x, int y, uint16_t fg, uint16_t bg, uint8_t size)
{
    chMtxLock(&mutex_ili9341);
    uint16_t *buf = spi_buffer;
    uint8_t bits;
    int c, r;
    for(c = 0; c < 7*size; c++) {
        bits = x5x7_bits[(ch * 7) + (c / size)];
        for (r = 0; r < 5*size; r++) {
            *buf++ = (0x80 & bits) ? fg : bg;
            if (r % size == (size-1)) {
                bits <<= 1;
            }
        }
    }
    ili9341_bulk(x, y, 5*size, 7*size);
    chMtxUnlock(&mutex_ili9341);
}

void ili9341_drawstring_size(const char *str, int x, int y, uint16_t fg, uint16_t bg, uint8_t size)
{
    chMtxLock(&mutex_ili9341);
    while (*str) {
        ili9341_drawchar_size(*str, x, y, fg, bg, size);
        x += 5 * size;
        str++;
    }
    chMtxUnlock(&mutex_ili9341);
}

#else

void ili9341_drawchar_7x13(uint8_t ch, int x, int y, uint16_t fg, uint16_t bg)
{
  uint16_t *buf = spi_buffer;
  uint16_t bits;
  int c, r;
  for(c = 0; c < 13; c++) {
    bits = x7x13b_bits[(ch * 13) + c];
    for (r = 0; r < 7; r++) {
      *buf++ = (0x8000 & bits) ? fg : bg;
      bits <<= 1;
    }
  }
  ili9341_bulk(x, y, 7, 13);
}

void
ili9341_drawstring_7x13(const char *str, int x, int y, uint16_t fg, uint16_t bg)
{
chMtxLock(&mutex_ili9341);
  while (*str) {
    ili9341_drawchar_7x13(*str, x, y, fg, bg);
    x += 7;
    str++;
  }
chMtxUnlock(&mutex_ili9341);
}


void
ili9341_drawchar_size(uint8_t ch, int x, int y, uint16_t fg, uint16_t bg, uint8_t size)
{
  chMtxLock(&mutex_ili9341);
  uint16_t *buf = spi_buffer;
  uint16_t bits;
  int c, r;
  for(c = 0; c < 13*size; c++) {
    bits = x7x13b_bits[(ch * 13) + (c / size)];
    for (r = 0; r < 7*size; r++) {
      *buf++ = (0x8000 & bits) ? fg : bg;
      if (r % size == (size-1)) {
          bits <<= 1;
      }
    }
  }
  ili9341_bulk(x, y, 7*size, 13*size);
chMtxUnlock(&mutex_ili9341);
}

void
ili9341_drawstring_size(const char *str, int x, int y, uint16_t fg, uint16_t bg, uint8_t size)
{
    chMtxLock(&mutex_ili9341);
  while (*str) {
    ili9341_drawchar_size(*str, x, y, fg, bg, size);
    x += 7 * size;
    str++;
  }
  chMtxUnlock(&mutex_ili9341);
}
#endif


#define SWAP(x,y) { int z=x; x = y; y = z; }

void ili9341_line(int x0, int y0, int x1, int y1, uint16_t fg)
{
    chMtxLock(&mutex_ili9341);
    if (x0 > x1) {
        SWAP(x0, x1);
        SWAP(y0, y1);
    }

    while (x0 <= x1) {
        int dx = x1 - x0 + 1;
        int dy = y1 - y0;
        if (dy >= 0) {
            dy++;
            if (dy > dx) {
                dy /= dx; dx = 1;
            } else {
                dx /= dy; dy = 1;
            }
        } else {
            dy--;
            if (-dy > dx) {
                    dy /= dx; dx = 1;
            } else {
                dx /= -dy; dy = -1;
            }
        }
        if (dy > 0)
            ili9341_fill(x0, y0, dx, dy, fg);
        else
            ili9341_fill(x0, y0+dy, dx, -dy, fg);
        x0 += dx;
        y0 += dy;
    }
    chMtxUnlock(&mutex_ili9341);
}


const font_t NF20x22 = { 20, 22, 1, 3*22, (const uint8_t *)numfont20x22 };

void ili9341_drawfont(uint8_t ch, const font_t *font, int x, int y, uint16_t fg, uint16_t bg)
{
    chMtxLock(&mutex_ili9341);
    uint16_t *buf = spi_buffer;
    const uint8_t *bitmap = &font->bitmap[font->slide * ch];
    int c, r;

    for (c = 0; c < font->height; c++) {
        uint8_t bits = *bitmap++;
        uint8_t m = 0x80;
        for (r = 0; r < font->width; r++) {
            *buf++ = (bits & m) ? fg : bg;
            m >>= 1;

            if (m == 0) {
                bits = *bitmap++;
                m = 0x80;
            }
        }
    }
    ili9341_bulk(x, y, font->width, font->height);
    chMtxUnlock(&mutex_ili9341);
}

#if 0
static const uint16_t colormap[] = {
    RGBHEX(0x00ff00), RGBHEX(0x0000ff), RGBHEX(0xff0000),
    RGBHEX(0x00ffff), RGBHEX(0xff00ff), RGBHEX(0xffff00)
};

static void ili9341_pixel(int x, int y, int color)
{
    uint8_t xx[4] = { x >> 8, x, (x+1) >> 8, (x+1) };
    uint8_t yy[4] = { y >> 8, y, (y+1) >> 8, (y+1) };
    uint8_t cc[2] = { color >> 8, color };
    send_command(0x2A, 4, xx);
    send_command(0x2B, 4, yy);
    send_command(0x2C, 2, cc);
    //send_command16(0x2C, color);
}

void ili9341_test(int mode)
{
    chMtxLock(&mutex_ili9341);
    int x, y;
    int i;
    switch (mode) {
        default:
#if 1
            ili9341_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
            for (y = 0; y < LCD_HEIGHT; y++) {
                ili9341_fill(0, y, LCD_WIDTH, 1, RGB(LCD_HEIGHT-y, y, (y + 120) % 256));
            }
            break;
        case 1:
            ili9341_fill(0, 0, LCD_WIDTH, LCD_HEIGHT, 0);
            for (y = 0; y < LCD_HEIGHT; y++) {
                for (x = 0; x < LCD_WIDTH; x++) {
                    ili9341_pixel(x, y, (y<<8)|x);
                }
            }
            break;
        case 2:
            //send_command16(0x55, 0xff00);
            ili9341_pixel(64, 64, 0xaa55);
            break;
#endif
#if 1
  case 3:
    for (i = 0; i < 10; i++)
      ili9341_drawfont(i, &NF20x22, i*20, 120, colormap[i%6], 0x0000);
    break;
#endif
#if 0
  case 4:
    draw_grid(10, 8, 29, 29, 15, 0, 0xffff, 0);
    break;
#endif
        case 4:
            ili9341_line(0, 0, 15, 100, 0xffff);
            ili9341_line(0, 0, 100, 100, 0xffff);
            ili9341_line(0, 15, 100, 0, 0xffff);
            ili9341_line(0, 100, 100, 0, 0xffff);
            break;
    }
    chMtxUnlock(&mutex_ili9341);
}
#endif

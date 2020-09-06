/*
 * Copyright (C) 2020 Gwenhael Goavec-Merou <gwenhael.goavec-merou@trabucayre.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <libusb.h>
#include <stdio.h>
#include <string.h>

#include <iostream>
#include <map>
#include <vector>
#include <string>

#include "display.hpp"
#include "usbBlaster.hpp"
#include "ftdipp_mpsse.hpp"

using namespace std;

#define DO_READ  (1 << 6)
#define DO_WRITE (0 << 6)
#define DO_RDWR  (1 << 6)
#define DO_SHIFT (1 << 7)
#define DO_BITBB (0 << 7)
#define DEFAULT ((1<<2) | (1<<3) | (1 << 5))

#define DEBUG 0

#ifdef DEBUG
#define display(...) \
	do { \
		if (_verbose) fprintf(stdout, __VA_ARGS__); \
	}while(0)
#else
#define display(...) do {}while(0)
#endif

UsbBlaster::UsbBlaster(bool verbose):
			_verbose(verbose), _nb_bit(0),
			_curr_tms(0), _buffer_size(64)
{
	init_internal();
}

UsbBlaster::~UsbBlaster()
{
	_in_buf[_nb_bit++] = 0;
	flush();
	free(_in_buf);
}

void UsbBlaster::init_internal()
{
	int ret;
	_ftdi = ftdi_new();
	if (_ftdi == NULL) {
		cout << "open_device: failed to initialize ftdi" << endl;
		throw std::exception();
	}

     ret = ftdi_usb_open(_ftdi, 0x09fb, 0x6001);
    if (ret < 0) {
		fprintf(stderr, "unable to open ftdi device: %d (%s)\n",
			ret, ftdi_get_error_string(_ftdi));
		ftdi_free(_ftdi);
		throw std::exception();
	}

	ret = ftdi_usb_reset(_ftdi);
	if (ret < 0) {
		fprintf(stderr, "Error reset: %d (%s)\n",
			ret, ftdi_get_error_string(_ftdi));
		ftdi_free(_ftdi);
		throw std::exception();
	}

	ret = ftdi_set_latency_timer(_ftdi, 2);
	if (ret < 0) {
		fprintf(stderr, "Error set latency timer: %d (%s)\n",
			ret, ftdi_get_error_string(_ftdi));
		ftdi_free(_ftdi);
		throw std::exception();
	}
 
	_tck_pin = (1 << 0);
	_tms_pin = (1 << 1);
	_tdi_pin = (1 << 4);

	_in_buf = (unsigned char *)malloc(sizeof(unsigned char) * _buffer_size);

	/* Force flush internal FT245 internal buffer */
	uint8_t val = DEFAULT | DO_WRITE | DO_BITBB | _tms_pin;
	uint8_t tmp_buf[4096];
	for (_nb_bit = 0; _nb_bit < 4096; _nb_bit += 2) {
		tmp_buf[_nb_bit    ] = val;
		tmp_buf[_nb_bit + 1] = val | _tck_pin;
	}

	ftdi_write_data(_ftdi, tmp_buf, _nb_bit);

	_nb_bit = 0;
	memset(_in_buf, 0, _buffer_size);
}

int UsbBlaster::setClkFreq(uint32_t clkHZ)
{
	(void) clkHZ;
	return 1;
}

int UsbBlaster::writeTMS(uint8_t *tms, int len, bool flush_buffer)
{
	int ret;

	/* nothing to send
	 * but maybe need to flush internal buffer
	 */
	if (len == 0) {
		if (flush_buffer) {
			ret = flush();
			return ret;
		}
		return 0;
	}

	/* check for at least one bit space in buffer */
	if (_nb_bit+2 > _buffer_size) {
		ret = flush();
		if (ret < 0)
			return ret;
	}

	/* fill buffer to reduce USB transaction */
	for (int i = 0; i < len; i++) {
		_curr_tms = ((tms[i >> 3] & (1 << (i & 0x07)))? _tms_pin : 0);
		uint8_t val = DEFAULT | DO_WRITE | DO_BITBB | _tdi_pin | _curr_tms;
		_in_buf[_nb_bit++] = val;
		_in_buf[_nb_bit++] = val | _tck_pin;

		if (_nb_bit + 2 > _buffer_size) {
			ret = flush();
			if (ret < 0)
				return ret;
		}
	}
	_in_buf[_nb_bit++] = DEFAULT | DO_WRITE | DO_BITBB | _curr_tms;

	/* security check: try to flush buffer */
	if (flush_buffer) {
		ret = flush();
		if (ret < 0)
			return ret;
	}
	//printInfo("writeTMS: end");

	return len;
}

#include "configBitstreamParser.hpp"

int UsbBlaster::writeTDI(uint8_t *tx, uint8_t *rx, uint32_t len, bool end)
{

	uint32_t real_len = (end) ? len -1 : len;
	uint32_t nb_byte = real_len >> 3;
	uint32_t nb_bit = (real_len & 0x07);
	uint8_t mode = (rx != NULL)? DO_RDWR : DO_WRITE;

	uint8_t *tx_ptr = tx;
	uint8_t *rx_ptr = rx;

	/* security: send residual
	 * it's possible since all functions do a flush at the end
	 * okay it's maybe less efficient
	 */
	_in_buf[_nb_bit++] = DEFAULT | DO_BITBB | DO_WRITE | _curr_tms;
	flush();

	if (_curr_tms == 0 && nb_byte != 0) {
		uint8_t mask = DO_SHIFT | mode;

		while (nb_byte != 0) {
			uint32_t tx_len = nb_byte;
			if (tx_len > 63)
				tx_len = 63;
			/* if not enough space flush */
			if (_nb_bit + tx_len + 1 > 64) {
				int num_read = _nb_bit -1;
				if (writeByte((rx)? rx_ptr:NULL, num_read) < 0)
					return -EXIT_FAILURE;
				if (rx)
					rx_ptr += num_read;
			}
			_in_buf[_nb_bit++] = mask | (tx_len & 0x3f);
			if (tx) {
				memcpy(&_in_buf[_nb_bit], tx_ptr, tx_len);
				tx_ptr += tx_len;
			} else {
				memset(&_in_buf[_nb_bit], 0, tx_len);
			}
			_nb_bit += tx_len;

			nb_byte -= tx_len;
		}

		if (_nb_bit != 0) {
			int num_read = _nb_bit-1;
			if (writeByte((rx)? rx_ptr:NULL, num_read) < 0)
				return -EXIT_FAILURE;
			if (rx)
				rx_ptr += num_read;
		}
	}

	if (nb_bit != 0) {
		uint8_t mask = DEFAULT | DO_BITBB;
		if (_nb_bit + 2 > _buffer_size) {
			int num_read = _nb_bit;
			if (writeBit((rx)? rx_ptr:NULL, num_read/2) < 0)
				return -EXIT_FAILURE;
			if (rx)
				rx_ptr += num_read;
		}
		for (uint32_t i = 0; i < nb_bit; i++) {
			uint8_t val = 0;
			if (tx)
				val |= ((tx_ptr[i >> 3] & (1 << (i & 0x07)))? _tdi_pin : 0);
			_in_buf[_nb_bit++] = mask | val;
			_in_buf[_nb_bit++] = mask | mode | val | _tck_pin;
		}

		int num_read = _nb_bit;
		if (writeBit((rx)? rx_ptr:NULL, num_read/2) < 0)
			return -EXIT_FAILURE;
	}

	/* set TMS high */
	if (end) {
		//printf("end\n");
		_curr_tms = _tms_pin;
		uint8_t mask = DEFAULT | DO_BITBB | _curr_tms;
		if (tx && *tx_ptr & (1 << nb_bit))
			mask |= _tdi_pin;
		_in_buf[_nb_bit++] = mask;
		_in_buf[_nb_bit++] = mask | mode | _tck_pin;
		uint8_t tmp;
		if (writeBit((rx)? &tmp:NULL, 1) < 0)
			return -EXIT_FAILURE;
		if (rx)
			*rx_ptr |= ((tmp & 0x80) << (7 - nb_bit));
		_in_buf[_nb_bit++] = mask | mode;
		if (writeBit(NULL, 0) < 0)
			return -EXIT_FAILURE;
	}

	return len;
}

int UsbBlaster::toggleClk(uint8_t tms, uint8_t tdi, uint32_t clk_len)
{
	int xfer_len = clk_len;

	int mask = DO_SHIFT | DO_WRITE;

	/* try to use shift mode but only when
	 * xfer > 1Byte and tms is low
	 */
	if (tms == 0 && xfer_len >= 8) {
		_in_buf[_nb_bit++] = DEFAULT | DO_WRITE | DO_BITBB;
		flush();
		/* fill a byte with all 1 or all 0 */
		uint8_t content = (tdi)?0xff:0;

		while (xfer_len >= 8) {
			uint16_t tx_len = (xfer_len >> 3);
			if (tx_len > 63)
				tx_len = 63;
			/* if not enough space flush */
			if (_nb_bit + tx_len + 1 > 64)
				if (flush() < 0)
					return -EXIT_FAILURE;
			_in_buf[_nb_bit++] = mask | static_cast<uint8_t>(tx_len);
			for (int i = 0; i < tx_len; i++)
				_in_buf[_nb_bit++] = content;
			xfer_len -= (tx_len << 3);
		}
	}

	mask = DEFAULT | DO_BITBB | DO_WRITE | ((tms) ? _tms_pin : 0) | ((tdi) ? _tdi_pin : 0);
	while (xfer_len > 0) {
		if (_nb_bit + 2 > _buffer_size)
			if (flush() < 0)
				return -EXIT_FAILURE;
		_in_buf[_nb_bit++] = mask;
		_in_buf[_nb_bit++] = mask | _tck_pin;

		xfer_len--;
	}

	/* flush */
	_in_buf[_nb_bit++] = mask;
	flush();

	return clk_len;
}

int UsbBlaster::flush()
{
	return write(false, 0);
}

/* simply call write and return buffer
 */
int UsbBlaster::writeByte(uint8_t *tdo, int nb_byte)
{
	int ret = write(tdo != NULL, nb_byte);	
	if (tdo && ret > 0)
		memcpy(tdo, _in_buf, nb_byte);
	return ret;
}

/* call write with a temporary buffer
 * if tdo reconstruct message
 */
int UsbBlaster::writeBit(uint8_t *tdo, int nb_bit)
{
	int ret = write(tdo != NULL, nb_bit);
	if (tdo && ret > 0) {
		/* need to reconstruct received word 
		 * since jtag is LSB first we need to shift right content by 1
		 * and add 0x80 (1 << 7) or 0
		 * the buffer may contains some tms bit, so start with i
		 * equal to fill exactly nb_bit bits
		 * */
		for (int i = 0, offset=0; i < nb_bit; i++, offset++) {
			tdo[offset >> 3] = (((_in_buf[i] & (1<<0)) ? 0x80 : 0x00) |
							(tdo[offset >> 3] >> 1));
		}
	}

	return ret;
}

int UsbBlaster::write(bool read, int rd_len)
{
	int ret = 0;
	if (_nb_bit == 0)
		return 0;

	ret = ftdi_write_data(_ftdi, _in_buf, _nb_bit);
	if (ret != _nb_bit) {
		printf("problem %d written %d\n", ret, _nb_bit);
		return ret;
	}

	if (read) {
		int timeout = 100;
		uint8_t byte_read = 0;
		while (byte_read < rd_len && timeout != 0) {
			timeout--;
			ret = ftdi_read_data(_ftdi, _in_buf + byte_read, rd_len - byte_read);
			if (ret < 0) {
				printError("Write error: " + std::to_string(ret));
				return ret;
			}
			byte_read += ret;
		}

		if (timeout == 0) {
			printError("Error: timeout " + std::to_string(byte_read) +
				" " + std::to_string(rd_len));
			for (int i=0; i < byte_read; i++)
				printf("%02x ", _in_buf[i]);
			printf("\n");
			return 0;
		}
	}
	_nb_bit = 0;
	return ret;
}

/*****************************************************************************
 Copyright(c) 2012 FCI Inc. All Rights Reserved
 
 File name : fci_i2c.c
 
 Description : fci i2c driver
*******************************************************************************/
#include <linux/mutex.h>
#include <linux/delay.h>

#include "fci_types.h"
#include "fci_oal.h"
#include "fc8150_regs.h"
#include "fci_hal.h"

static DEFINE_MUTEX(fci_tuner_lock);

#define FEATURE_SIMPLE_INTERFACE

#define I2CSTAT_TIP         0x02 /* Tip bit */
#define I2CSTAT_NACK        0x80 /* Nack bit */

#define I2C_TIMEOUT         100

#define I2C_CR_STA          0x80
#define I2C_CR_STO          0x40
#define I2C_CR_RD           0x20
#define I2C_CR_WR           0x10
#define I2C_CR_NACK         0x08
#define I2C_CR_IACK         0x01

#define I2C_WRITE           0
#define I2C_READ            1

#define I2C_OK              0
#define I2C_NOK             1
#define I2C_NACK            2
#define I2C_NOK_LA          3 /* Lost arbitration */
#define I2C_NOK_TOUT        4 /* time out */

//#define  FC8150_FREQ_XTAL  BBM_XTAL_FREQ
extern u32 bbm_xtal_freq;

//static OAL_SEMAPHORE hBbmMutex;

static int WaitForXfer (HANDLE hDevice)
{
	int res = I2C_OK;
#ifdef FEATURE_SIMPLE_INTERFACE
	udelay(30);
#else
	int i;
	u8 status;

	i = I2C_TIMEOUT * 20000;
	// wait for transfer complete
	do {
		bbm_read(hDevice, BBM_I2C_SR, &status);
		i--;
	} while ((i > 0) && (status & I2CSTAT_TIP));

	// check time out or nack
	if(status & I2CSTAT_TIP) {
		res = I2C_NOK_TOUT;
	} else {
		bbm_read(hDevice, BBM_I2C_SR, &status);
		if(status & I2CSTAT_NACK) res = I2C_NACK;
		else res = I2C_OK;
	}
#endif
	return res;
}

static int fci_i2c_transfer (HANDLE hDevice, u8 cmd_type, u8 chip, u8 addr[], u8 addr_len, u8 data[], u8 data_len)
{
	int i;
	int result = I2C_OK;
#ifdef FEATURE_SIMPLE_INTERFACE
	u16 cmd;
#endif

	switch (cmd_type) {
		case I2C_WRITE:
#ifdef FEATURE_SIMPLE_INTERFACE
			cmd = (I2C_CR_STA | I2C_CR_WR);
			cmd = (cmd<<8) | (chip | cmd_type);
			bbm_word_write(hDevice, BBM_I2C_TXR, cmd);
#else
			bbm_write(hDevice, BBM_I2C_TXR, chip | cmd_type);
			bbm_write(hDevice, BBM_I2C_CR, I2C_CR_STA | I2C_CR_WR /*0x90*/);
#endif
			result = WaitForXfer(hDevice);
			if(result != I2C_OK) return result;

			if (addr && addr_len) {
				i = 0;
				while ((i < addr_len) && (result == I2C_OK)) {
#ifdef FEATURE_SIMPLE_INTERFACE
					cmd = (I2C_CR_WR);
					cmd = (cmd<<8) | (addr[i]);
					bbm_word_write(hDevice, BBM_I2C_TXR, cmd);
#else
					bbm_write(hDevice, BBM_I2C_TXR, addr[i]);
					bbm_write(hDevice, BBM_I2C_CR, I2C_CR_WR /*0x10*/);
#endif
					result = WaitForXfer(hDevice);
					if(result != I2C_OK) return result;
					i++;
				}
			}

			i = 0;
			while ((i < data_len) && (result == I2C_OK)) {
#ifdef FEATURE_SIMPLE_INTERFACE
				cmd = (I2C_CR_WR);
				cmd = (cmd<<8) | (data[i]);
				bbm_word_write(hDevice, BBM_I2C_TXR, cmd);
#else
				bbm_write(hDevice, BBM_I2C_TXR, data[i]);
				bbm_write(hDevice, BBM_I2C_CR, I2C_CR_WR /*0x10*/);
#endif

				result = WaitForXfer(hDevice);
				if(result != I2C_OK) return result;
				i++;
			}

			bbm_write(hDevice, BBM_I2C_CR, I2C_CR_STO /*0x40*/);
			result = WaitForXfer(hDevice);
			if(result != I2C_OK) return result;
			break;
		case I2C_READ:
			if (addr && addr_len) {
#ifdef FEATURE_SIMPLE_INTERFACE
				cmd = (I2C_CR_STA | I2C_CR_WR);
				cmd = (cmd<<8) | (chip | I2C_WRITE);
				bbm_word_write(hDevice, BBM_I2C_TXR, cmd);
#else
				bbm_write(hDevice, BBM_I2C_TXR, chip | I2C_WRITE);
				bbm_write(hDevice, BBM_I2C_CR, I2C_CR_STA | I2C_CR_WR /*0x90*/); // send start
#endif
				result = WaitForXfer(hDevice);
				if(result != I2C_OK) {
					return result;
				}

				i = 0;
				while ((i < addr_len) && (result == I2C_OK)) {
#ifdef FEATURE_SIMPLE_INTERFACE
					cmd = (I2C_CR_WR);
					cmd = (cmd<<8) | (addr[i]);
					bbm_word_write(hDevice, BBM_I2C_TXR, cmd);
#else
					bbm_write(hDevice, BBM_I2C_TXR, addr[i]);
					bbm_write(hDevice, BBM_I2C_CR, I2C_CR_WR /*0x10*/);
#endif
					result = WaitForXfer(hDevice);
					if(result != I2C_OK) {
						return result;
					}
					i++;
				}
			}
#ifdef FEATURE_SIMPLE_INTERFACE
			cmd = (I2C_CR_STA | I2C_CR_WR);
			cmd = (cmd<<8) | (chip | I2C_READ);
			bbm_word_write(hDevice, BBM_I2C_TXR, cmd);
#else
			bbm_write(hDevice, BBM_I2C_TXR, chip | I2C_READ);
			bbm_write(hDevice, BBM_I2C_CR, I2C_CR_STA | I2C_CR_WR /*0x90*/); // resend start
#endif

			result = WaitForXfer(hDevice);
			if(result != I2C_OK) {
				return result;
			}

			i = 0;
			while ((i < data_len) && (result == I2C_OK)) {
				if (i == data_len - 1) {
					bbm_write(hDevice, BBM_I2C_CR, I2C_CR_RD|I2C_CR_NACK/*0x28*/);	// No Ack Read
					result = WaitForXfer(hDevice);
					if((result != I2C_NACK) && (result != I2C_OK)){
						PRINTF(hDevice, "NACK4-0[%02x]\n", result);
						return result;
					}
				} else {
					bbm_write(hDevice, BBM_I2C_CR, I2C_CR_RD /*0x20*/);	// Ack Read
					result = WaitForXfer(hDevice);
					if(result != I2C_OK){
						PRINTF(hDevice, "NACK4-1[%02x]\n", result);
						return result;
					}
				}
				bbm_read(hDevice, BBM_I2C_RXR, &data[i]);
				i++;
			}

			bbm_write(hDevice, BBM_I2C_CR, I2C_CR_STO /*0x40*/);		// send stop
			result = WaitForXfer(hDevice);
			if((result != I2C_NACK) && (result != I2C_OK)) {
				PRINTF(hDevice, "NACK5[%02X]\n", result);
				return result;
			}
			break;
		default:
			return I2C_NOK;
	}

	return I2C_OK;
}

int fci_i2c_init (HANDLE hDevice, int speed, int slaveaddr)
{
	u16 r = bbm_xtal_freq % (5 * speed);
	u16 pr = (bbm_xtal_freq - r) / (5 * speed) - 1;
	if(((5 * speed) >> 1) <= r)
		pr++;


	bbm_word_write(hDevice, BBM_I2C_PR_L, pr);
	bbm_write(hDevice, BBM_I2C_CTR, 0xc0);

	return BBM_OK;
}

int fci_i2c_read(HANDLE hDevice, u8 chip, u8 addr, u8 alen, u8 *data, u8 len)
{
	int ret;

	mutex_lock(&fci_tuner_lock);
	ret = fci_i2c_transfer(hDevice, I2C_READ, chip << 1, &addr, alen, data, len);
	mutex_unlock(&fci_tuner_lock);
	if(ret != I2C_OK) {
		PRINTF(hDevice, "fci_i2c_read() result=%d, addr = %x, data=%x\n", ret, addr, *data);
		return ret;
	}

	return ret;
}

int fci_i2c_write(HANDLE hDevice, u8 chip, u8 addr, u8 alen, u8 *data, u8 len)
{
	int ret;
	u8 *paddr = &addr;

	mutex_lock(&fci_tuner_lock);
	ret = fci_i2c_transfer(hDevice, I2C_WRITE, chip << 1, paddr, alen, data, len);
	mutex_unlock(&fci_tuner_lock);

	if(ret != I2C_OK) {
		PRINTF(hDevice, "fci_i2c_write() result=%d, addr= %x, data=%x\n", ret, addr, *data);
	}

	return ret;
}

int fci_i2c_deinit(HANDLE hDevice)
{
	bbm_write(hDevice, BBM_I2C_CTR, 0x00);

	return BBM_OK;
}


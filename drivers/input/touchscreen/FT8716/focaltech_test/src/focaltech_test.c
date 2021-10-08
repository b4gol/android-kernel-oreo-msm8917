/*
 *
 * FocalTech TouchScreen driver.
 *
 * Copyright (c) 2010-2017, FocalTech Systems, Ltd., all rights reserved.
 * Copyright (C) 2018 XiaoMi, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/************************************************************************
*
* File Name: focaltech_test.c
*
* Author:     Software Department, FocalTech
*
* Created: 2016-08-01
*
* Modify:
*
* Abstract: create char device and proc node for  the comm between APK and TP
*
************************************************************************/

/*****************************************************************************
* Included header files
*****************************************************************************/
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <asm/uaccess.h>

#include <linux/i2c.h>
#include <linux/delay.h>

#include "../../focaltech_core.h"
#include "../include/focaltech_test_main.h"
#include "../include/focaltech_test_ini.h"

#include <linux/proc_fs.h>
#include <linux/fs.h>
#define CTP_PARENT_PROC_NAME  "touchscreen"
#define CTP_OPEN_PROC_NAME        "ctp_openshort_test"

#if FTS_LOCK_DOWN_INFO
static char tp_lockdown_info[128];
#define FTS_PROC_LOCKDOWN_FILE "lockdown_info"
static int fts_lockdown_proc_show(struct seq_file *file, void *data)
{
	char temp[40] = {0};

	sprintf(temp, "%s\n", tp_lockdown_info);
	seq_printf(file, "%s\n", temp);

	return 0;
}

static int fts_lockdown_proc_open (struct inode *inode, struct file *file)
{
	return single_open(file, fts_lockdown_proc_show, inode->i_private);
}

static const struct file_operations fts_lockdown_proc_fops = {
	.open = fts_lockdown_proc_open,
	.read = seq_read,
};

#endif

#if FTS_CAT_RAWDATA
#define FTS_PROC_RAWDATA  "rawdata"
static u8 g_isdiff = 1;
/************************************************************************
* Name: StartScan(Same function name as FT_MultipleTest)
* Brief:  Scan TP, do it before read Raw Data
* Input: none
* Output: none
* Return: Comm Code. Code = 0x00 is OK, else fail.
***********************************************************************/
static int StartScan(struct i2c_client *client)
{
	u8 RegVal = 0x00;
	u8 times = 0;
	const u8 MaxTimes = 20;
	u8 ReCode = -1;

	ReCode = fts_i2c_write_reg(client, 0x00, 0xC0);
	if (ReCode >= 0) {
		while (times++ < MaxTimes) {
			msleep(8);
			ReCode = fts_i2c_read_reg(client, 0x00, &RegVal);
			if (RegVal == 0x40)
				break;
		}

		if (times > MaxTimes)
			return -EPERM;
	}
	return ReCode;
}

int read_rawdata(struct i2c_client *client, int is_diff, s16 *data, int len)
{
	u8 reg = 0x6A;
	u8 regdata[1280] = { 0 };
	int remain_bytes;
	int pos = 0;
	int i = 0;

	FTS_DEBUG("len=%d, is_diff=%d", len, is_diff);
	fts_i2c_write_reg(client, 0x06, is_diff);

	if (StartScan(client) < 0)
		return -EPERM;

	fts_i2c_write_reg(client, 0x01, 0xAD);

	if (len <= 256)
		fts_i2c_read(client, &reg, 1, regdata, len);
	else {
		fts_i2c_read(client, &reg, 1, regdata, 256);
		remain_bytes = len - 256;
		for (i = 1; remain_bytes > 0; i++) {
			if (remain_bytes > 256)
				fts_i2c_read(client, &reg, 0, regdata + i * 256, 256);
			else
				fts_i2c_read(client, &reg, 0, regdata + i * 256, remain_bytes);
			remain_bytes -= 256;
		}
	}

	for (i = 0; i < len;) {
		data[pos++] = ((s16)(regdata[i]) << 8) + regdata[i+1];
		i += 2;
	}
	return 0;
}

int get_rawdata(struct i2c_client *client, s16 *data, u8 *txlen, u8 *rxlen)
{
	u8 val;
	int i;

	/* 0xEE = 1, not clb */
	fts_i2c_write_reg(client, 0xEE, 1);

	/* Enter Factory Mode */
	fts_i2c_write_reg(client, 0x00, 0x40);
	do {
		fts_i2c_read_reg(client, 0x00, &val);
		if (val == 0x40)
			break;
		msleep(1);
	} while (i < 10);

	/* Get Tx/Rx Num */
	fts_i2c_read_reg(client, 0x02, txlen);
	fts_i2c_read_reg(client, 0x03, rxlen);

	/* read_rawdata */
	read_rawdata(client, g_isdiff, data, (*txlen) * (*rxlen) * 2);

	/* Enter in work mode */
	fts_i2c_write_reg(client, 0x00, 0x00);
	do {
		fts_i2c_read_reg(client, 0x00, &val);
		if (val == 0x00)
			break;
		msleep(1);
	} while (i < 10);

	return 0;
}

static char temp[PAGE_SIZE] = {0};

static int fts_rawdata_proc_show(struct seq_file *file, void *buf)
{
	int count;
	int i = 0, j = 0;
	u8 val;
	s16 data[600] = { 0 };
	u8 txlen = 0;
	u8 rxlen = 0;
	struct i2c_client *client = fts_i2c_client;

	if (fts_wq_data->suspended) {
		FTS_INFO("Already in suspend state");
		return -EPERM;
	}
	mutex_lock(&fts_input_dev->mutex);

	get_rawdata(client, data, &txlen, &rxlen);

	fts_i2c_read_reg(client, 0xEE, &val);
	count = snprintf(temp, PAGE_SIZE, "0xEE = %d\n", val);
	count += snprintf(temp + count, PAGE_SIZE-count, "%s :\n", g_isdiff ? "DIFF DATA" : "RAWDATA");
	for (i = 0; i < txlen; i++) {
		for (j = 0; j < rxlen; j++) {
			count += snprintf(temp + count, PAGE_SIZE-count, "%5d ", data[i*rxlen + j]);
		}
		count += snprintf(temp + count, PAGE_SIZE-count, "\n");
	}
	count += snprintf(temp + count, PAGE_SIZE-count, "\n\n");
	seq_printf(file, "%s\n", temp);
	memset(temp, 0, PAGE_SIZE);
	mutex_unlock(&fts_input_dev->mutex);

	return 0;
}

static int fts_rawdata_proc_open (struct inode *inode, struct file *file)
{
	return single_open(file, fts_rawdata_proc_show, inode->i_private);
}

static ssize_t fts_rawdata_proc_store(struct file *file, const char __user *buf, size_t count, loff_t *len)
{
	unsigned long val;
	int ret;
	char *buff[5] = {0};
	if (fts_wq_data->suspended) {
		FTS_INFO("Already in suspend state");
		return -EPERM;
	}
	ret = copy_from_user(buff, buf, 5);
	mutex_lock(&fts_input_dev->mutex);
	val = simple_strtoul((char *)buff, NULL, 10);
	if (val)
		g_isdiff = 1;
	else
		g_isdiff = 0;
	mutex_unlock(&fts_input_dev->mutex);

	return count;
}
static const struct file_operations fts_rawdata_proc_fops = {
	.open = fts_rawdata_proc_open,
	.read = seq_read,
	.write = fts_rawdata_proc_store,
};
#endif

static ssize_t ctp_open_proc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos);
static ssize_t ctp_open_proc_write(struct file *filp, const char __user *userbuf, size_t count, loff_t *ppos);
static const struct file_operations ctp_open_procs_fops = {
	.write = ctp_open_proc_write,
	.read = ctp_open_proc_read,
	.owner = THIS_MODULE,
};
static struct proc_dir_entry *ctp_device_proc;
/*****************************************************************************
* Private constant and macro definitions using #define
*****************************************************************************/
#define IC_TEST_VERSION  "Test version: V1.0.0--2016-12-28, (sync version of FT_MultipleTest: V4.0.0.0 ------ 2016-07-18)"


#define FTS_INI_FILE_PATH "/etc/"

#define FTS_SAVE_DATA_FILE_PATH "/mnt/sdcard/"


#define FTS_TEST_BUFFER_SIZE        80*1024
#define FTS_TEST_PRINT_SIZE     128
/*****************************************************************************
* Private enumerations, structures and unions using typedef
*****************************************************************************/


/*****************************************************************************
* Static variables
*****************************************************************************/

/*****************************************************************************
* Global variable or extern global variabls/functions
*****************************************************************************/


/*****************************************************************************
* Static function prototypes
*****************************************************************************/
static int fts_test_get_ini_size(char *config_name);
static int fts_test_read_ini_data(char *config_name, char *config_buf);
static int fts_test_save_test_data(char *file_name, char *data_buf, int iLen);
static int fts_test_get_testparam_from_ini(char *config_name);
static int fts_test_entry(char *ini_file_name);

static int fts_test_i2c_read(unsigned char *writebuf, int writelen, unsigned char *readbuf, int readlen);
static int fts_test_i2c_write(unsigned char *writebuf, int writelen);


/*****************************************************************************
* functions body
*****************************************************************************/
#if 0

extern struct i2c_client *fts_i2c_client;
extern int fts_i2c_read(struct i2c_client *client, char *writebuf, int writelen, char *readbuf, int readlen);
extern int fts_i2c_write(struct i2c_client *client, char *writebuf, int writelen);
#endif
static int fts_test_i2c_read(unsigned char *writebuf, int writelen, unsigned char *readbuf, int readlen)
{
	int iret = -1;
#if 1


	iret = fts_i2c_read(fts_i2c_client, writebuf, writelen, readbuf, readlen);
#else
	iret = fts_i2c_read(writebuf, writelen, readbuf, readlen);
#endif

	return iret;

}

static int fts_test_i2c_write(unsigned char *writebuf, int writelen)
{
	int iret = -1;
#if 1


	iret = fts_i2c_write(fts_i2c_client, writebuf, writelen);
#else
	iret = fts_i2c_write(writebuf, writelen);
#endif

	return iret;
}


static int fts_test_get_ini_size(char *config_name)
{
	struct file *pfile = NULL;
	struct inode *inode = NULL;

	off_t fsize = 0;
	char filepath[128];

	FTS_TEST_FUNC_ENTER();

	memset(filepath, 0, sizeof(filepath));

	sprintf(filepath, "%s%s", FTS_INI_FILE_PATH, config_name);

	if (NULL == pfile)
		pfile = filp_open(filepath, O_RDONLY, 0);

	if (IS_ERR(pfile)) {
		FTS_TEST_ERROR("error occured while opening file %s.",  filepath);
		return -EIO;
	}

	inode = pfile->f_dentry->d_inode;

	fsize = inode->i_size;
	filp_close(pfile, NULL);

	FTS_TEST_FUNC_ENTER();

	return fsize;
}


static int fts_test_read_ini_data(char *config_name, char *config_buf)
{
	struct file *pfile = NULL;
	struct inode *inode = NULL;

	off_t fsize = 0;
	char filepath[128];
	loff_t pos = 0;
	mm_segment_t old_fs;

	FTS_TEST_FUNC_ENTER();

	memset(filepath, 0, sizeof(filepath));
	sprintf(filepath, "%s%s", FTS_INI_FILE_PATH, config_name);
	if (NULL == pfile) {
		pfile = filp_open(filepath, O_RDONLY, 0);
	}
	if (IS_ERR(pfile)) {
		FTS_TEST_ERROR("error occured while opening file %s.",  filepath);
		return -EIO;
	}

	inode = pfile->f_dentry->d_inode;

	fsize = inode->i_size;
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	pos = 0;
	vfs_read(pfile, config_buf, fsize, &pos);
	filp_close(pfile, NULL);
	set_fs(old_fs);

	FTS_TEST_FUNC_EXIT();
	return 0;
}


static int fts_test_save_test_data(char *file_name, char *data_buf, int iLen)
{
	struct file *pfile = NULL;

	char filepath[128];
	loff_t pos;
	mm_segment_t old_fs;

	FTS_TEST_FUNC_ENTER();

	memset(filepath, 0, sizeof(filepath));
	sprintf(filepath, "%s%s", FTS_SAVE_DATA_FILE_PATH, file_name);
	if (NULL == pfile) {

		pfile = filp_open(filepath, O_TRUNC|O_CREAT|O_RDWR, 0);
	}
	if (IS_ERR(pfile)) {
		FTS_TEST_ERROR("error occured while opening file %s.",  filepath);
		return -EIO;
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	pos = 0;
	vfs_write(pfile, data_buf, iLen, &pos);
	filp_close(pfile, NULL);
	set_fs(old_fs);

	FTS_TEST_FUNC_EXIT();
	return 0;
}


static int fts_test_get_testparam_from_ini(char *config_name)
{
	char *pcfiledata = NULL;
	int ret = 0;
	int inisize = 0;

	FTS_TEST_FUNC_ENTER();

	inisize = fts_test_get_ini_size(config_name);
	FTS_TEST_DBG("ini_size = %d ", inisize);
	if (inisize <= 0) {
		FTS_TEST_ERROR("%s ERROR:Get firmware size failed",  __func__);
		return -EIO;
	}

	pcfiledata = fts_malloc(inisize + 1);
	if (NULL == pcfiledata) {
		FTS_TEST_ERROR("fts_malloc failed in function:%s",  __func__);
		return -EPERM;
	}

	memset(pcfiledata, 0, inisize + 1);

	if (fts_test_read_ini_data(config_name, pcfiledata)) {
		FTS_TEST_ERROR(" - ERROR: fts_test_read_ini_data failed");
		fts_free(pcfiledata);
		pcfiledata = NULL;

		return -EIO;
	} else {
		FTS_TEST_DBG("fts_test_read_ini_data successful");
	}

	ret = set_param_data(pcfiledata);

	fts_free(pcfiledata);
	pcfiledata = NULL;

	FTS_TEST_FUNC_EXIT();

	if (ret < 0)
		return ret;

	return 0;
}




static int fts_test_entry(char *ini_file_name)
{
	/* place holder for future use */
	char cfgname[128];
	char *testdata = NULL;
	char *printdata = NULL;
	int iTestDataLen = 0;
	int ret = 0;
	int icycle = 0, i = 0;
	int print_index = 0;


	FTS_TEST_FUNC_ENTER();
	FTS_TEST_DBG("ini_file_name:%s.", ini_file_name);
	/*Used to obtain the test data stored in the library, pay attention to the size of the distribution space.*/
	FTS_TEST_DBG("Allocate memory, size: %d", FTS_TEST_BUFFER_SIZE);
	testdata = fts_malloc(FTS_TEST_BUFFER_SIZE);
	if (NULL == testdata) {
		FTS_TEST_ERROR("fts_malloc failed in function:%s",  __func__);
		return -EPERM;
	}
	printdata = fts_malloc(FTS_TEST_PRINT_SIZE);
	if (NULL == printdata) {
		FTS_TEST_ERROR("fts_malloc failed in function:%s",  __func__);
		return -EPERM;
	}
	/*Initialize the platform related I2C read and write functions*/

#if 0
	init_i2c_write_func(fts_i2c_write);
	init_i2c_read_func(fts_i2c_read);
#else
	init_i2c_write_func(fts_test_i2c_write);
	init_i2c_read_func(fts_test_i2c_read);
#endif

	/*Initialize pointer memory*/
	ret = focaltech_test_main_init();
	if (ret < 0) {
		FTS_TEST_ERROR("focaltech_test_main_init() error.");
		goto TEST_ERR;
	}

	/*Read parse configuration file*/
	memset(cfgname, 0, sizeof(cfgname));
	sprintf(cfgname, "%s", ini_file_name);
	FTS_TEST_DBG("ini_file_name = %s", cfgname);

	fts_test_funcs();

	if (fts_test_get_testparam_from_ini(cfgname) < 0) {
		FTS_TEST_ERROR("get testparam from ini failure");
		goto TEST_ERR;
	}


	if ((g_ScreenSetParam.iSelectedIC >> 4  != FTS_CHIP_TEST_TYPE >> 4)) {
		FTS_TEST_ERROR("Select IC and Read IC from INI does not match ");

	}


	/*Start testing according to the test configuration*/
	if (true == start_test_tp()) {
		TestResultLen += sprintf(TestResult+TestResultLen, "Tp test pass. \n\n");
		FTS_TEST_INFO("tp test pass");
		ret = 0;
	}

	else {
		TestResultLen += sprintf(TestResult+TestResultLen, "Tp test failure. \n\n");
		FTS_TEST_INFO("tp test failure");
		ret = 1;
	}


	/*Gets the number of tests in the test library and saves it*/
	iTestDataLen = get_test_data(testdata);


	icycle = 0;
	/*Print test data packets */
	FTS_TEST_DBG("print test data: \n");
	for (i = 0; i < iTestDataLen; i++) {
		if (('\0' == testdata[i])
			|| (icycle == FTS_TEST_PRINT_SIZE - 2)
			|| (i == iTestDataLen-1)
			  ) {
			if (icycle == 0) {
				print_index++;
			} else {
				memcpy(printdata, testdata + print_index, icycle);
				printdata[FTS_TEST_PRINT_SIZE-1] = '\0';
				FTS_TEST_DBG("%s", printdata);
				print_index += icycle;
				icycle = 0;
			}
		} else {
			icycle++;
		}
	}
	FTS_TEST_DBG("\n");



	fts_test_save_test_data("testdata.csv", testdata, iTestDataLen);
	fts_test_save_test_data("testresult.txt", TestResult, TestResultLen);


	/*Release memory */
	focaltech_test_main_exit();



	if (NULL != testdata)
		fts_free(testdata);
	if (NULL != printdata)
		fts_free(printdata);

	FTS_TEST_FUNC_EXIT();

	return ret;

TEST_ERR:
	if (NULL != testdata)
		fts_free(testdata);
	if (NULL != printdata)
		fts_free(printdata);

	FTS_TEST_FUNC_EXIT();

	return -EPERM;
}

static int fts_set_ini_name(char *cfgname)
{
	int ret;
	u8 vid;

	ret = fts_i2c_read_reg(fts_i2c_client, FTS_REG_VENDOR_ID, &vid);
	FTS_TEST_DBG("vendor id:0x%x\n", vid);
	if (vid == OFILM_VENDOR) {
	sprintf(cfgname, "%s", "fts_ofilm.ini");
	} else if (vid == EACHOPTO_VENDOR) {
	sprintf(cfgname, "%s", "fts_eachopto.ini");
	} else if (vid == TXD_VENDOR) {

	sprintf(cfgname, "%s", "fts_holitech.ini");
	} else if (vid == EBBG_VENDOR) {
	sprintf(cfgname, "%s", "fts_ebbg.ini");
	} else if (vid == CSOT_VENDOR) {
	sprintf(cfgname, "%s", "fts_csot.ini");
	} else{
	pr_err("ctp test not found test config \n");
	}

	return ret;
}

#if FTS_AUTO_UPGRADE_EN
void waite_for_fw_upgrading(void)
{
	int vid = 0;
	if (fw_upgrade_status == FW_UPGRADING) {
		for (; fw_upgrade_status == FW_UPGRADING; ) {

			if ((vid%4) == 0)
				FTS_TEST_DBG("tp firmware upgrading status:%d\n", fw_upgrade_status);
			msleep(200);
			vid++;
			if (vid > 30) {
				FTS_TEST_DBG("takes 6s tp still upgrading status:%d\n", fw_upgrade_status);
				break;
			}
		}
	}
	return;
}
#endif

static ssize_t ctp_open_proc_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
		char *ptr = buf;
		char fwname[128] = {0};
		int ret;
		u8 result = 0;
		struct i2c_client *client = fts_i2c_client;

		FTS_TEST_FUNC_ENTER();
#if FTS_AUTO_UPGRADE_EN
	waite_for_fw_upgrading();
#endif

	if (*ppos) {
		FTS_TEST_ERROR("tp test again return\n");
		return 0;
	}
	*ppos += count;

		memset(fwname, 0, sizeof(fwname));
		fts_set_ini_name(fwname);
		fwname[strlen(fwname)] = '\0';
		FTS_TEST_DBG("fwname:%s.", fwname);

		mutex_lock(&fts_input_dev->mutex);

		disable_irq(client->irq);

#if defined(FTS_ESDCHECK_EN) && (FTS_ESDCHECK_EN)
		fts_esdcheck_switch(DISABLE);
#endif
		ret = fts_test_entry(fwname);
		if (0 != ret) {
			result = 0;
			FTS_TEST_ERROR("fts open short test fail \n");
		} else {
			result = 1;
			FTS_TEST_ERROR("fts open short test success \n");
		}
#if defined(FTS_ESDCHECK_EN) && (FTS_ESDCHECK_EN)
		fts_esdcheck_switch(ENABLE);
#endif
		enable_irq(client->irq);

		mutex_unlock(&fts_input_dev->mutex);

		FTS_TEST_FUNC_EXIT();

	return sprintf(ptr, "result=%d\n", result);
}


static ssize_t ctp_open_proc_write(struct file *filp, const char __user *userbuf, size_t count, loff_t *ppos)
{
	return -EPERM;
}
 void create_ctp_proc(void)
{





	struct proc_dir_entry *ctp_open_proc = NULL;
	if (ctp_device_proc == NULL) {
		ctp_device_proc = proc_mkdir(CTP_PARENT_PROC_NAME, NULL);
		if (ctp_device_proc == NULL) {
			FTS_TEST_ERROR("create parent_proc fail\n");
			return;
		}
	}
	ctp_open_proc = proc_create(CTP_OPEN_PROC_NAME, 0777, ctp_device_proc, &ctp_open_procs_fops);
	if (ctp_open_proc == NULL) {
		FTS_TEST_ERROR("create open_proc fail\n");
	}
}
#if 0
/************************************************************************
* Name: fts_test_show
* Brief:  no
* Input: device, device attribute, char buf
* Output: no
* Return: EPERM
***********************************************************************/
static ssize_t fts_test_show(struct device *dev, struct device_attribute *attr, char *buf)
{
		char fwname[128] = {0};
		int ret;
		struct i2c_client *client = fts_i2c_client;

		FTS_TEST_FUNC_ENTER();

#if FTS_AUTO_UPGRADE_EN
	waite_for_fw_upgrading();
#endif
		memset(fwname, 0, sizeof(fwname));
		fts_set_ini_name(fwname);
		fwname[strlen(fwname)] = '\0';
		FTS_TEST_DBG("fwname:%s.", fwname);

		mutex_lock(&fts_input_dev->mutex);

		disable_irq(client->irq);

#if defined(FTS_ESDCHECK_EN) && (FTS_ESDCHECK_EN)
		fts_esdcheck_switch(DISABLE);
#endif
		ret = fts_test_entry(fwname);

#if defined(FTS_ESDCHECK_EN) && (FTS_ESDCHECK_EN)
		fts_esdcheck_switch(ENABLE);
#endif
		enable_irq(client->irq);

		mutex_unlock(&fts_input_dev->mutex);

		FTS_TEST_FUNC_EXIT();

	return sprintf(buf, "result=%d\n", ret);
}

/************************************************************************
* Name: fts_test_store
* Brief:  upgrade from app.bin
* Input: device, device attribute, char buf, char count
* Output: no
* Return: char count
***********************************************************************/
static ssize_t fts_test_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{

	return count;
}
/*  upgrade from app.bin
*    example:echo "***.ini" > fts_test
*/
static DEVICE_ATTR(fts_test, S_IRUGO|S_IWUSR, fts_test_show, fts_test_store);

/* add your attr in here*/
static struct attribute *fts_test_attributes[] = {
	&dev_attr_fts_test.attr,
	NULL
};

static struct attribute_group fts_test_attribute_group = {
	.attrs = fts_test_attributes
};
#endif

int fts_test_init(struct i2c_client *client)
{
	int err = 0;

	FTS_TEST_FUNC_ENTER();

	FTS_TEST_INFO("%s\n",  IC_TEST_VERSION);

	create_ctp_proc();

	FTS_TEST_FUNC_EXIT();

	return err;
}
int fts_test_exit(struct i2c_client *client)
{
	FTS_TEST_FUNC_ENTER();


	FTS_TEST_FUNC_EXIT();
	return 0;
}

#if	FTS_LOCK_DOWN_INFO
int fts_lockdown_init(struct i2c_client *client)
{
	int err = 0;
	unsigned char auc_i2c_write_buf[10];
	u8  r_buf[10] = {0};
	struct proc_dir_entry *fts_lockdown_status_proc = NULL;
	err = fts_i2c_write_reg(client, 0x90, 0x20);
	if (err < 0)
		FTS_ERROR("[FTS] i2c write 0x90 err\n");

	msleep(5);
	auc_i2c_write_buf[0] = 0x99;
	err = fts_i2c_read(client, auc_i2c_write_buf, 1, r_buf, 8);
	if (err < 0)
		FTS_ERROR("[FTS] i2c read 0x99 err\n");

	sprintf(tp_lockdown_info, "%02x%02x%02x%02x%02x%02x%02x%02x", \
			r_buf[0], r_buf[1], r_buf[2], r_buf[3], r_buf[4], r_buf[5], r_buf[6], r_buf[7]);
	FTS_INFO("tpd_probe, ft8716_ctpm_LockDownInfo_get_from_boot, tp_lockdown_info=%s\n", tp_lockdown_info);

	if (ctp_device_proc == NULL) {
		ctp_device_proc = proc_mkdir(CTP_PARENT_PROC_NAME, NULL);
		if (ctp_device_proc == NULL) {
			FTS_TEST_ERROR("create parent_proc fail\n");
			return 1;
		}
	}
	fts_lockdown_status_proc = proc_create(FTS_PROC_LOCKDOWN_FILE, 0644, ctp_device_proc, &fts_lockdown_proc_fops);
	if (fts_lockdown_status_proc == NULL) {
		FTS_ERROR("fts, create_proc_entry ctp_lockdown_status_proc failed\n");
		return 1;
	}
	return 0 ;
}
#endif

#if FTS_CAT_RAWDATA
int fts_rawdata_init(struct i2c_client *client)
{

	struct proc_dir_entry *fts_rawdata_status_proc = NULL;


	if (ctp_device_proc == NULL) {
		ctp_device_proc = proc_mkdir(CTP_PARENT_PROC_NAME, NULL);
		if (ctp_device_proc == NULL) {
			FTS_TEST_ERROR("create parent_proc fail\n");
			return 1;
		}
	}
	fts_rawdata_status_proc = proc_create(FTS_PROC_RAWDATA, 0777, ctp_device_proc, &fts_rawdata_proc_fops);
	if (fts_rawdata_status_proc == NULL) {
		FTS_ERROR("fts, create_proc_entry ctp_lockdown_status_proc failed\n");
		return 1;
	}
	return 0 ;
}
#endif

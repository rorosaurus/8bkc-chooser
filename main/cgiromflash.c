/*
Some flash handling cgi routines. Used for updating the appfs.
*/

/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain 
 * this notice you can do whatever you want with this stuff. If we meet some day, 
 * and you think this stuff is worth it, you can buy me a beer in return. 
 * ----------------------------------------------------------------------------
 */

#include <esp8266.h>
#include "cgiromflash.h"
#include "espfs.h"
#include "cgiflash.h"
#include "espfs.h"
#include "esp_partition.h"

//#include <osapi.h>
#include "cgiflash.h"
#include "espfs.h"
#include "httpd-platform.h"
#include "esp32_flash.h"
#include "appfs.h"

#ifndef UPGRADE_FLAG_FINISH
#define UPGRADE_FLAG_FINISH     0x02
#endif



//Cgi that allows the firmware to be replaced via http POST This takes
//a direct POST from e.g. Curl or a Javascript AJAX call with either the
//firmware given by cgiGetFirmwareNext or an OTA upgrade image.

//Because we don't have the buffer to allocate an entire sector but will 
//have to buffer some data because the post buffer may be misaligned, we 
//write SPI data in pages. The page size is a software thing, not
//a hardware one.
#define PAGELEN 4096

#define FLST_START 0
#define FLST_WRITE 1
#define FLST_DONE 3
#define FLST_ERROR 4

typedef struct {
	appfs_handle_t fd;
	int state;
	int filetype;
	int flashPos;
	char pageData[PAGELEN];
	int pagePos;
	int address;
	int len;
	char *err;
} UploadState;


int ICACHE_FLASH_ATTR cgiUploadRom(HttpdConnData *connData) {
	UploadState *state=(UploadState *)connData->cgiData;

	if (connData->conn==NULL) {
		//Connection aborted. Clean up.
		if (state!=NULL) free(state);
		return HTTPD_CGI_DONE;
	}

	if (state==NULL) {
		//First call. Allocate and initialize state variable.
		httpd_printf("Firmware upload cgi start.\n");
		state=malloc(sizeof(UploadState));
		if (state==NULL) {
			httpd_printf("Can't allocate firmware upload struct!\n");
			return HTTPD_CGI_DONE;
		}
		memset(state, 0, sizeof(UploadState));
		state->state=FLST_START;
		connData->cgiData=state;
		state->err="Premature end";
	}
	
	char *data=connData->post->buff;
	int dataLen=connData->post->buffLen;
	
	while (dataLen!=0) {
		if (state->state==FLST_START) {
			const char name[512];
			int len=httpdFindArg(connData->getArgs, "name", name, sizeof(name));
			if (len<=0) {
				state->err="No name";
				state->state=FLST_ERROR;
			}
			esp_err_t err=appfsCreateFile(name, connData->post->len, &state->fd);
			if (err!=ESP_OK) {
				state->err="App too large for free space";
				state->state=FLST_ERROR;
			} else {
				state->len=connData->post->len;
				state->address=0;
				state->state=FLST_WRITE;
			}
		} else if (state->state==FLST_WRITE) {
			//Copy bytes to page buffer, and if page buffer is full, flash the data.
			//First, calculate the amount of bytes we need to finish the page buffer.
			int lenLeft=PAGELEN-state->pagePos;
			if (state->len<lenLeft) lenLeft=state->len; //last buffer can be a cut-off one
			//See if we need to write the page.
			if (dataLen<lenLeft) {
				//Page isn't done yet. Copy data to buffer and exit.
				memcpy(&state->pageData[state->pagePos], data, dataLen);
				state->pagePos+=dataLen;
				state->len-=dataLen;
				dataLen=0;
			} else {
				//Finish page; take data we need from post buffer
				memcpy(&state->pageData[state->pagePos], data, lenLeft);
				data+=lenLeft;
				dataLen-=lenLeft;
				state->pagePos+=lenLeft;
				state->len-=lenLeft;
				//Erase sector, if needed
				if ((state->address&(SPI_FLASH_SEC_SIZE-1))==0) {
					appfsErase(state->fd, state->address, SPI_FLASH_SEC_SIZE);
				}
				//Write page
				httpd_printf("Writing %d bytes of data to SPI pos 0x%x...\n", state->pagePos, state->address);
				appfsWrite(state->fd, state->address, (uint8_t*)state->pageData, state->pagePos);
				state->address+=PAGELEN;
				state->pagePos=0;
				if (state->len==0) state->state=FLST_DONE;
			}
		} else if (state->state==FLST_DONE) {
			httpd_printf("Huh? %d bogus bytes received after data received.\n", dataLen);
			//Ignore those bytes.
			dataLen=0;
		} else if (state->state==FLST_ERROR) {
			//Just eat up any bytes we receive.
			dataLen=0;
		}
	}
	
	if (connData->post->len==connData->post->received) {
		//We're done! Format a response.
		httpd_printf("Upload done. Sending response.\n");
		httpdStartResponse(connData, state->state==FLST_ERROR?400:200);
		httpdHeader(connData, "Content-Type", "text/plain");
		httpdEndHeaders(connData);
		if (state->state!=FLST_DONE) {
			httpdSend(connData, "Upload error:", -1);
			httpdSend(connData, state->err, -1);
			httpdSend(connData, "\n", -1);
		}
		free(state);
		return HTTPD_CGI_DONE;
	}
	return HTTPD_CGI_MORE;
}

int ICACHE_FLASH_ATTR cgiRomIdx(HttpdConnData *connData) {
	int *idx=(int*)&connData->cgiData;
	char buff[128];
	int r;
	int fd;

	printf("cgiRomIdx: run %d\n", *idx-0x100);
	if (*idx==0) {
		httpdStartResponse(connData, 200);
		httpdHeader(connData, "Content-Type", "text/json");
		httpdEndHeaders(connData);
		httpdSend(connData, "{\n\"roms\": [\n", -1);
		fd=APPFS_INVALID_FD;
	} else {
		fd=*idx-0x100;
	}

	printf("Grabbing entry after %d\n", fd);
	fd=appfsNextEntry(fd);
	printf("Next appfs fd: %d\n", fd);
	if (fd!=APPFS_INVALID_FD) {
		char *name=NULL;
		int size;
		appfsEntryInfo(fd, &name, &size);
		sprintf(buff, "%s{\"index\": %d, \"name\": \"%s\", \"size\": %d, \"addr\": \"0x%X\" }\n", *idx?",":"", 
			fd, name, size, 0);
		printf(" - %s\n", buff);
		httpdSend(connData, buff, -1);
		*idx=fd+0x100;
		return HTTPD_CGI_MORE;
	} else {
		httpdSend(connData, "],\n\"free\": ", -1);
		sprintf(buff, "%d", appfsGetFreeMem());
		printf(" - %s\n", buff);
		httpdSend(connData, buff, -1);
		httpdSend(connData, "}\n", -1);
		return HTTPD_CGI_DONE;
	}
}

int ICACHE_FLASH_ATTR cgiDelete(HttpdConnData *connData) {
		httpdStartResponse(connData, 200);
		httpdHeader(connData, "Content-Type", "text/html");
		httpdEndHeaders(connData);
		httpdSend(connData, "Killed", -1);
//	romspaceKillAll();
	return HTTPD_CGI_DONE;
}

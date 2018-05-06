#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "payload.h"
#include "firm_old.h"
#include "firm_new.h"

#define VERSION "4.0.0"
#define RWCHUNK	(3072*512) //3072 sectors (1.5 MB)
#define RWMINI	(128*512) //64 KB

const u8 SHA1OLD[20]={0x08,0x3B,0x81,0x7A,0x36,0x42,0x68,0x85,0x15,0xB1,0x76,0x3D,0x3B,0xD8,0xC3,0x6F,0x1B,0x30,0x85,0x22};
const u8 SHA1NEW[20]={0xA2,0xF3,0x36,0x71,0xDE,0xC5,0xD7,0xF3,0xA4,0xC2,0x01,0xD4,0x98,0xA3,0x90,0x65,0x0B,0x2D,0x82,0x9B};
const u8 SHA1B9S[20]={0xBF,0x91,0x19,0x46,0xB2,0x42,0x63,0x7C,0x11,0x05,0xCC,0x6B,0xD3,0xF2,0x81,0x58,0xBC,0xC6,0xE2,0xD1};
u32 foffset=0x0B130000 / 0x200;
u32 hoffset=(0x0B130000 / 0x200) + (0x800000/0x200) - 1;

//Function index________________________
int main();
int checkNCSD();
//void dump3dsNand(int mode);
//void restore3dsNand(int mode);
void xorbuff(u8 *in1, u8 *in2, u8 *out);
void installB9S(u32 firmtype);
u32 handleUI();
u32 waitNandWriteDecision();
u32 getMBremaining();
int getFirmBuf(u32 len);
int file2buf(char *path, u8 *buf, u32 limit);
int createBackup();
int verifyBackup();
int checkA9LH();
int verifyUnlockKey(char *path);
void error(int code);
//______________________________________

const char *green="\x1b[32;1m";
const char *yellow="\x1b[33;1m";
const char *blue="\x1b[34;1m";
const char *dblue="\x1b[34;0m";
const char *white="\x1b[37;1m";

int menu_index=0;
u32 MB=0x100000;
u32 KB=0x400;
u32 sysid=0;  //NCSD magic
u32 ninfo=0;  //nand size in sectors
u32 sizeMB=0; //nand/firm size in MB
u32 remainMB=0;
u32 System=0; //will be one of the below 2 variables
int b9s_install_count=0; //don't let user do this more than once!
u32 N3DS=2;
u32 O3DS=1;
u32 B9S=2;
u32 STOCK=1;
u32 firmStatus=0;
u64 frame=0;
char workdir[]= "boot9strap";
char nand_type[80]={0};   //sd filename buffer for nand/firm dump/restore.
u8 *workbuffer; //raw dump and restore in first two options
u8 *fbuff; //sd firm files
u8 *xbuff; //xorpad
u8 *nbuff; //nand
PrintConsole topScreen;
PrintConsole bottomScreen;

int main() {
	
	videoSetModeSub(MODE_0_2D); //first 7 lines are all dual screen printing init taken from libnds's nds-examples
	videoSetMode(MODE_0_2D);   
	vramSetBankC(VRAM_C_SUB_BG);
	vramSetBankA(VRAM_A_MAIN_BG);
	consoleInit(&bottomScreen, 3,BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
	consoleInit(&topScreen, 3,BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleSelect(&topScreen); 
	int res=0;
	
    iprintf("Initializing FAT... ");
	if (!fatInitDefault()) error(0);
	printf("good\n");
	iprintf("Initializing NAND... ");
	if(!nand_Startup()) error(5);
	printf("good\n");
	workbuffer = (u8*)malloc(RWCHUNK);  //setup and allocate our buffers. 
	if(!workbuffer) error(4);
	fbuff = (u8*)malloc(RWMINI);
	nbuff = (u8*)malloc(RWMINI);
	xbuff = (u8*)malloc(RWMINI);
	
	remainMB=getMBremaining();
	
	mkdir(workdir, 0777);   //boot9strap folder creation
	chdir(workdir);
	checkNCSD();     //read ncsd header for needed info
	
	dumpUnlockKey();
	checkA9LH();
	res = verifyBackup();
	iprintf("Verify backup res: %08X\n", res);
	if(res==1){
		iprintf("Creating BACKUP.BIN...\n");
		res = createBackup();
		iprintf("Create backup res: %08X\n", res);
		if(res) error(9);
		res = verifyBackup();
		iprintf("Verify backup res: %08X\n", res);
	}
	
	if(res) error(7);
	if(firmStatus != B9S && firmStatus != STOCK) error(6);
	
	int wait=60;
	while(wait--)swiWaitForVBlank();
	while(handleUI());  //game loop

	systemShutDown();
	return 0;
}

int checkNCSD() {
	nand_ReadSectors(0 , 1 , workbuffer);   //get NCSD (nand) header of the 3ds
	memcpy(&sysid, workbuffer + 0x100, 4);  //NCSD magic
	memcpy(&ninfo, workbuffer + 0x104, 4);  //nand size in sectors (943 or 1240 MB). used to determine old/new 3ds.
	if     (ninfo==0x00200000){ System=O3DS;}    //old3ds
	else if(ninfo==0x00280000){ System=N3DS;}    //new3ds
	if(!System || sysid != 0x4453434E)error(2);  //this error triggers if NCSD magic not present or nand size unexpected value.

	return 0;
}

void xorbuff(u8 *in1, u8 *in2, u8 *out){

	for(int i=0; i < RWMINI; i++){
		out[i] = in1[i] ^ in2[i];
	}

}

void installB9S(u32 firmtype) {
	u8 hash[20];
	u8 *header_offset = workbuffer + MB + 0x10000 - 0x200;
	int res;
	consoleClear();
	iprintf("%sWILL BRICK%s if A9LH is installed!", yellow, white); 
	iprintf("%sMAKE SURE%s your firmware matches\n", yellow, white);
	iprintf("the %sBLUE%s above!\n\n", blue, white);
	if(waitNandWriteDecision())return;
	consoleClear();
	
	if(firmtype==B9S){  //do opposite of firmware status
		nand_WriteSectors(foffset, MB/0x200, workbuffer);
		nand_ReadSectors(foffset, MB/0x200, workbuffer);
		swiSHA1Calc(hash,  workbuffer, MB);
		res = memcmp(hash, header_offset+0x20, 20);
		iprintf("Result: %08X %s\n", res, res ? "HASH FAIL":"HASH GOOD!");
		iprintf("%d KBs written to FIRM0\r", (int)(MB/1024));
	}
	else if(firmtype==STOCK){
		nand_WriteSectors(foffset, payload_len/0x200, workbuffer+MB);
		nand_ReadSectors(foffset, payload_len/0x200, workbuffer+MB);
		swiSHA1Calc(hash,  workbuffer+MB, payload_len);
		res = memcmp(hash, header_offset+0x40, 20);
		iprintf("Result: %08X %s\n", res, res ? "HASH FAIL":"HASH GOOD!");
		iprintf("%d KBs written to FIRM0\r", payload_len / 1024);
	}

	iprintf("\nDone!\n");
	error(99); //not really an error, we just don't want multiple nand writes in one session.
}

u32 handleUI(){
	consoleSelect(&topScreen);
	consoleClear();
	const char status[3][64]={
		{"never see me I hope"},
		{"STOCK"}, //aka clean firm
		{"B9S"},
	};
	
	char action[64];
	sprintf(action,"%s\n",(firmStatus==B9S) ? "Restore stock firmware":"Install boot9strap");

	const int menu_size=2;
	char menu[2][64];
	strcpy(menu[0],"Exit\n");
	strcpy(menu[1],action);

	iprintf("b9sTool %s | %ldMB free\n", VERSION, remainMB); 
	if    (System==O3DS)iprintf("%sOLD 3DS%s\n", yellow, white);
	else                iprintf("%sNEW 3DS%s\n", yellow, white);
	iprintf("%s%s%s\n", frame%60<15 ? dblue:blue, firmwareNames, white);
	iprintf("%s\nFIRM STATUS: %s%s\n\n", green, white, status[firmStatus]);

	for(int i=0;i<menu_size;i++){
		iprintf("%s%s\n", i==menu_index ? " > " : "   ", menu[i]);
	}

	swiWaitForVBlank();
	scanKeys();
	int keypressed=keysDown();

	if     (keypressed & KEY_DOWN)menu_index++;
	else if(keypressed & KEY_UP)  menu_index--;
	else if(keypressed & KEY_A){
		consoleSelect(&bottomScreen);
		if     (menu_index==0)return 0;          //break game loop and exit app
		else if(menu_index==1)installB9S(firmStatus);
		//else if(menu_index==2)installB9S(STOCK);
		//else if(menu_index==6)dump3dsNand(1);    // dump/restore full nand
		//else if(menu_index==3)restore3dsNand(1);
		//else if(menu_index==4)dump3dsNand(0);    // dump/restore firm0 and firm1
		//else if(menu_index==5)restore3dsNand(0);
	}
	if(menu_index >= menu_size)menu_index=0;     //menu index wrap around check
	else if(menu_index < 0)    menu_index=menu_size-1;

	frame++;
	return 1;  //continue game loop
}

u32 waitNandWriteDecision(){
	iprintf("NAND writing is DANGEROUS!\n");
	iprintf("START+SELECT = %sGO!%s\n", green, white);
	iprintf("B to decline.\n");
	while(1){
		scanKeys();
		int keys = keysHeld();
		if((keys & KEY_START) && (keys & KEY_SELECT))break;
		if(keys & KEY_B){
			consoleClear();
			return 1;
		}
		swiWaitForVBlank();
	}
	return 0;
}

u32 getMBremaining(){
	struct statvfs stats;
	statvfs("/", &stats);
	u64 total_remaining = ((u64)stats.f_bsize * (u64)stats.f_bavail) / (u64)0x100000;
	return (u32)total_remaining;
}

int getFirmBuf(u32 len){
	u8 digest[20];
	int res=0;
	
	iprintf("Loading b9s from RAM...\n");
	swiSHA1Calc(digest, payload, len);
	res = memcmp(SHA1B9S, digest, 20);
	if(res) return 0;
	
	iprintf("Loading Native Firm from RAM...\n");
	if(System==O3DS){
		swiSHA1Calc(digest, firm_old, len);
		res = memcmp(SHA1OLD, digest, 20);
		if(!res) return O3DS;
		printf("O3DS firm not in RAM trying SD\n");
		file2buf("2.54-0_11.4_OLD.firm", firm_old, payload_len);
		swiSHA1Calc(digest, firm_old, len);
		res = memcmp(SHA1OLD, digest, 20);
		if(!res) return O3DS;
	}
	else if(System==N3DS){
		swiSHA1Calc(digest, firm_new, len);
		res = memcmp(SHA1NEW, digest, 20);
		if(!res) return N3DS;
		printf("N3DS firm not in RAM trying SD\n");
		file2buf("2.54-0_11.4_NEW.firm", firm_new, payload_len);
		swiSHA1Calc(digest, firm_new, len);
		res = memcmp(SHA1NEW, digest, 20);
		if(!res) return N3DS;
	}
	
	iprintf("Native Firm not found, abort...\n");
	
	return 0;  //fail
}

int file2buf(char *path, u8 *buf, u32 limit){
	u32 size=0;
	FILE *f=fopen(path, "rb");
	if(!f) return 1;
	
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(size > limit) size = limit;
	
	fread(buf, 1, size, f);
	fclose(f);
	
	return 0;
}

int createBackup(){
	memset(workbuffer, 0, 0x110000);
	u8 hash1[20];
	u8 hash2[20];
	u8 *header_offset = workbuffer + MB + 0x10000 - 0x200;
	int res;
	//int count=0;
	iprintf("Loading clean firm ...\n");
	nand_ReadSectors(foffset, MB / 0x200, workbuffer);
	swiSHA1Calc(hash1, workbuffer, MB);
	nand_ReadSectors(foffset, MB / 0x200, workbuffer);
	swiSHA1Calc(hash2, workbuffer, MB);
	
	if(memcmp(hash1, hash2, 20)) return 1;
	memcpy(nbuff, workbuffer, payload_len);
	
	res = getFirmBuf(payload_len);  
	if(!res) return 2;

	if(res==O3DS){
		memcpy(fbuff, firm_old, payload_len);
	}
	else if(res==N3DS){
		memcpy(fbuff, firm_new, payload_len);
	}
	
	iprintf("Preparing crypted b9s firm...\n");
	
	xorbuff(fbuff,nbuff,xbuff);                             //xor the enc firm 11.4 with plaintext firm 11.4 to create xorpad buff
	memcpy(fbuff, payload, payload_len);                    //get payload
	xorbuff(fbuff,xbuff,nbuff);                             //xor payload and xorpad to create final encrypted image to write to destination
	memcpy(workbuffer + MB, nbuff, payload_len);	        //write it to destination
	
	swiSHA1Calc(hash2, workbuffer + MB, payload_len);
	
	iprintf("Creating header...\n");
	
	memcpy(header_offset, "B9ST", 4);                       //backup header - magic
	memcpy(header_offset + 4, &MB, 4);                      //firm len. this is always 1MB which should be plenty. hashing past len won't hurt.
	memcpy(header_offset + 8, &payload_len, 4);             //b9s payload len
	memcpy(header_offset + 0x20, hash1, 20);                //1MB clean firm hash
	memcpy(header_offset + 0x40, hash2, 20);                //b9s payload hash
	
	nand_WriteSectors(hoffset, 1, header_offset);           //store backup.bin header to nand, locking it to the system it was created on.
	
	iprintf("Dumping BACKUP.BIN to file...\n");
	
	FILE *f=fopen("BACKUP.BIN","wb");
	if(!f) return 3;
	fwrite(workbuffer, 1, MB + 0x10000, f);
	fclose(f);
	
	iprintf("Done!\n");
	
	return 0;
	
}

int verifyBackup(){
	u8 hash[20];
	u8 *header=(u8*)malloc(0x200);
	int res=0;
	
	nand_ReadSectors(hoffset, 1, header);
	//iprintf("check %s\n", header);
	if(memcmp("B9ST", header, 4)){
		return 1;
	}
	else{
		res = verifyUnlockKey("danger_reset_backup");
		if(!res){
			printf("Reset file found, recreating\nBACKUP.BIN...\n");
			return 1;
		}
	}
	
	nand_ReadSectors(foffset, MB/0x200, workbuffer);
	
	swiSHA1Calc(hash, workbuffer, *(u32*)(header+8));
	if(!memcmp(hash, header+0x40, 20)){
		firmStatus=B9S;
	}
	else{
		//swiSHA1Calc(hash, workbuffer, *(u32*)(header+4));
		//if(!memcmp(hash, header+0x20, 20))
		firmStatus=STOCK;
	}
	
	if(file2buf("BACKUP.BIN", workbuffer, 0x110000))return 2;
	if(memcmp(header, workbuffer + 0x110000-0x200, 0x200)) return 3;
	
	swiSHA1Calc(hash, workbuffer, *(u32*)(header+4));
	if(memcmp(hash, header+0x20, 20)) {
		//firmStatus=STOCK;
		printf("Stock firm hash bad\n");
		return 4;
	}
	swiSHA1Calc(hash, workbuffer+MB, *(u32*)(header+8));
	if(memcmp(hash, header+0x40, 20)) {
		//firmStatus=B9S;
		printf("B9S firm hash bad\n");
		return 5;
	}

	return 0;
}

int checkA9LH(){
	int res=0;
	iprintf("Checking for A9LH...\n");
	
	res = verifyUnlockKey("danger_skip_a9lh");  //override check if file found
	if(!res){
		iprintf("A9LH file found, overriding\nA9LH check. This is *very*\ndangerous!!\n");
		return 1;
	}
	
	nand_ReadSectors(0x5C000, 20*KB/0x200, workbuffer); 
	if(memmem(workbuffer, 20*KB, "arm9loaderhax.bin", 17) != 0) error(9);
	//nand offset 0xB800000 or FIRM1+0x2D0000
    //this is where plaintext stage2 a9lh payload is written
    //and the string "arm9loaderhax.bin" should be within 20KB of that offset
	
	return 0;
}

int verifyUnlockKey(char *path){
	FILE *f=fopen(path,"rb");
	if(!f){
		//iprintf("Unlock file read error\n");
		return 1;
	}
	fread(workbuffer, 1, 0x200, f);
	fclose(f);
	nand_ReadSectors(0, 1, workbuffer+0x200);
	
	if(memcmp(workbuffer, workbuffer+0x200, 0x200)) return 1;
	unlink(path);
	
	return 0;
}

int dumpUnlockKey(){
	FILE *f=fopen("key.bin","wb");
	if(!f){
		error(10);
	}
	nand_ReadSectors(0, 1, workbuffer);
	fwrite(workbuffer, 1, 0x200, f);
	fclose(f);
	return 0;
}

void error(int code){
	switch(code){
		case 0:  iprintf("Fat could not be initialized!\n"); break;
		case 1:  iprintf("Could not open file handle!\n"); break;
		case 2:  iprintf("Not a 3ds (or nand read error)!\n"); break;
		case 3:  iprintf("Could not open sdmc files!\n"); break;
		case 4:  iprintf("Workbuffer failed init!\n"); break;
		case 5:  iprintf("Nand failed init!\n"); break;
		case 6:  iprintf("Failed to load valid Firm from\nBACKUP.BIN\n"); break;
		case 7:  iprintf("Problem verifying BACKUP.BIN\n"); break;
		case 8:  iprintf("Create BACKUP.BIN failed!\n"); break;
		case 9:  iprintf("A9LH dectected! Brick avoided!!\nPlease install B9S with\nSafeB9SInstaller!\n"); break;
		case 10: iprintf("Unlock file read error\n"); break;
		case 11: iprintf("Unlock file verify error\n"); break;
		case 99:;
		default: break;
	}

	iprintf("\nPress home to exit\n");
	//free(workbuffer); free(fbuff); free(nbuff); free(xbuff);
	while(1)swiWaitForVBlank();
}
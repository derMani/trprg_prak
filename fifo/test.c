#include <stdio.h>

int main(int argc, char *argv[]) 
{
	FILE* f = fopen("/dev/fifo0", "wb");
	
	printf("8 Zeichen schreiben\n");
	for(int n = 0; n < 8; n++) {
		fputc('1' + n, f);
	}	
	fclose(f);
	
	f = fopen("/dev/fifo0", "rb");
	int ch = fgetc(f);
	while(ch != EOF) {
		printf("%c", ch);
		ch = fgetc(f);
	}
	printf(" gelesen\n");
	fclose(f);
	
	f = fopen("/dev/fifo0", "wb");
	for(int n = 0; n < 4; n++) {
		fputc('1' + n, f);
	}
	fclose(f);
	
	f = fopen("/dev/fifo0", "rb");
	ch = fgetc(f);
	printf("%c", ch);
	ch = fgetc(f);
	printf("%c gelesen\n", ch);
	fclose(f);
	
	f = fopen("/dev/fifo0", "wb");
	for(int n = 0; n < 6; n++) {
		fputc('5' + n, f);
	}
	fclose(f);
	
	f = fopen("/dev/fifo0", "rb");
	ch = fgetc(f);
	while(ch != EOF) {
		printf("%c", ch);
		ch = fgetc(f);
	}
	printf(" gelesen\n");
	fclose(f);
}
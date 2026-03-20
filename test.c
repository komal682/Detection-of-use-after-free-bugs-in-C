#include <stdio.h> 
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "memory.h"

#define MAX_PAGES 1000000
int *addr_arr[MAX_PAGES];

int main(int argc, char *argv[])
{
	int i, j, k;
	int ret = 0;
	int idx = 0;
	for (i = 0; i < MAX_PAGES; i++) {
		for (j = 0; j < 64; j++) {
			int *a = malloc(64);
			for (k = 0; k < 16; k++) {
				a[k] = k;
			}
			addr_arr[i] = a;
			if (j != idx) {
				free(a);
			}
		}
		idx = (idx + 1) % 64;
	}
	for (i = 0; i < MAX_PAGES; i++) {
		int *arr = addr_arr[i];
		ret += arr[i % 64];
	}
	printf("ret: %d\n", ret);
  return ret;
}

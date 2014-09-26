#include <tap.h>
#include <keyfile.h>
#include <my_aes.h>
#include <my_sys.h>

void
printAll(struct keyentry **all, int length)
{
    int i;
    for(i=0;i<length;i++) {
        if(NULL != all[i]) {
            printEntry(all[i]);
        }
    }
}

void
printEntry(struct keyentry *entry)
{
    printf("\nid:%d \niv:%s \nkey:%s", entry->id, entry->iv, entry->key);
}
void
test_parseFile_ciphertext(){
	plan(6);
	char *secret = "secret";
	struct keyentry **allKeys = (struct keyentry**) malloc( 256 * sizeof(struct keyentry*));
	FILE *fp = fopen("keys.enc", "r");
	if(0 == parseFile(fp, allKeys, 256, secret)) {
		fclose(fp);
		ok(allKeys[1]->id == 1, "Key id 1 is present");
		ok(!strcmp(allKeys[2]->iv,"35B2FF0795FB84BBD666DB8430CA214E"), "Testing IV value of key 2");
		ok(!strcmp(allKeys[15]->key, "B374A26A71490437AA024E4FADD5B497FDFF1A8EA6FF12F6FB65AF2720B59CCF"),"Testing key value of key 15");
		ok((allKeys[47] == 0), "Key id 47 should be null.");
		ok(allKeys[255]->id == 255, "Last key inserted");
		ok((allKeys[256] == 0), "Cannot insert more keys than defined.");
		ok(0 == strcmp(allKeys[4]->key, "770A8A65DA156D24EE2A093277530142770A8A65DA156D24EE2A093277530142"), "Parser ignores entries that are too long.");
	} else
	{
		ok(0, "Cannot open testfile");
	}
	free(allKeys);
}

void
test_parseFile_plaintext()
{
	plan(6);
	struct keyentry **allKeys = (struct keyentry**) malloc( 256 * sizeof(struct keyentry*));
	FILE *fp = fopen("keys.txt", "r");
	if(0 == parseFile(fp, allKeys, 256, NULL)) {
		//printAll(allKeys, 256);
		fclose(fp);
		ok(allKeys[1]->id == 1, "Key id 1 is present");
		ok(0 == strcmp(allKeys[2]->iv,"35B2FF0795FB84BBD666DB8430CA214E"), "Testing IV value of key 2");
		ok(0 == strcmp(allKeys[15]->key, "B374A26A71490437AA024E4FADD5B497FDFF1A8EA6FF12F6FB65AF2720B59CCF"),"Testing key value of key 15");
		ok((allKeys[47] == 0), "Key id 47 should be null.");
		ok(allKeys[255]->id == 255, "Last key inserted");
		ok((allKeys[256] == 0), "Cannot insert more keys than defined.");
		ok(0 == strcmp(allKeys[4]->key, "770A8A65DA156D24EE2A093277530142770A8A65DA156D24EE2A093277530142"), "Parser ignores entries that are too long.");
	} else {
		ok(0, "Cannot open testfile");
	}
	free(allKeys);
}

int
main(int argc __attribute__((unused)),char *argv[])
{
	test_parseFile_ciphertext();
	test_parseFile_plaintext();
	return 0;
}

/*
 * enc0enc.c
 *
 *  Created on: 22.08.2014
 *      Author: florin
 */

#include "enc0enc-t.h"
#include <stdlib.h>
#include <stdio.h>
#include <tap.h>

typedef unsigned long int	ulint;

extern int summef(int a, int b);
extern int summef2(int a, int b);
extern void fil_flush(ulint	space_id);


int main()
{
	plan(9);
	 //MY_INIT("enc0enc-t");
	printf("%s\n", "main() enc0enc.c");
	int sum = summef(2, 3);
	printf("%s, %d\n", "summef(int a, int b)", sum);
	//fil_flush(3);
	printf("%s, %d\n", "summef2(int a, int b)", summef2(2, 7));
/*
    string encrypted = encryptDecrypt("kylewbanks.com");
    cout << "Encrypted:" << encrypted << "\n";

    string decrypted = encryptDecrypt(encrypted);
    cout << "Decrypted:" << decrypted << "\n";
*/
	return 0;
}


/*
int summe(int a, int b)
{
	return a + b;
}

int summe(int a, int b);
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char * argv[]){

	// daca trimit doar fisierul, inseamna ca vreau sa criptez, 
	// daca trimit ca al doilea argument si fisierul cu cheia, inseamna ca vreau sa decriptez
	
	if(argc == 2){
		
		execve("encrypt", argv, NULL);
		perror("Eroare la apelarea encriptorului");	
	}
	else{
		execve("decrypt", argv, NULL);
		perror("Eroare la apelarea decriptorului");
	}
	
	return 0;
}

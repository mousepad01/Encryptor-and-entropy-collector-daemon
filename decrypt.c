#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>

// pentru debug si afisari in consola
const int NOTIFICATIONS = 1;

size_t WORD_SIZE = sizeof(long);
size_t PAGE_SIZE;

struct key{
	
	char * perm;
} KEY;	

void getkey(char * key_filename){
	
	int key_file_descriptor = open(key_filename, O_RDONLY);
	
	KEY.perm = malloc(WORD_SIZE * 8);
	char blank;
	
	read(key_file_descriptor, KEY.perm, WORD_SIZE);
}

void decrypt(char * m_ptr){

	if(NOTIFICATIONS)
		printf("Procesul copil %d decripteaza..\n", getpid());
	
	for(int w_count = 0; w_count < PAGE_SIZE / WORD_SIZE; w_count++){
		
		char encrypted_word = m_ptr[w_count];
		char decrypted_word = 0;
		
		for(int i = 0; i < WORD_SIZE; i++){
			
			decrypted_word |= ((encrypted_word >> KEY.perm[i]) & 1) << i;
		}
		
		m_ptr[w_count] = decrypted_word;
	}		
}
	
int main(int argc, char * argv[]){

	PAGE_SIZE = getpagesize();
	
	// deschiderea fisierului si preluarea file descriptor ului

	char * filename = argv[1];
	char * key_filename = argv[2];
	
	int file_descriptor = open(filename, O_RDWR);
	
	if(file_descriptor < 0){
		
		perror("Eroare la deschiderea fisierului");
		return errno;
	}
	
	// determinarea dimensiunii fisierului
	
	struct stat st;
	if(fstat(file_descriptor, &st) < 0){
		
		perror("Nu s-a putut prelua dimensiunea fisierului");
		return errno;
	}
	
	off_t file_size = st.st_size;
	
	if(NOTIFICATIONS)
		printf("file size %ld\n", file_size);
	
	// aflarea numarului de pagini de memorie mapata necesare
	
	int n_pages = file_size / PAGE_SIZE;
	
	if(file_size % PAGE_SIZE)
		n_pages += 1;
		
	if(NOTIFICATIONS)
		printf("number of pages %d\n", n_pages);
		
	// preluarea cheii din fisier
	
	getkey(key_filename);
	
	int key_file_descriptor = open("keyfile.txt", O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	
	for(int i = 0; i < WORD_SIZE; i++){
		
		write(key_file_descriptor, &KEY.perm[i], 1);
		write(key_file_descriptor, " ", 1);
	}
	
	// decriptarea cu ajutorul mai multor procese
	
	for(int proc_cnt = 0; proc_cnt < n_pages; proc_cnt++){
	
		pid_t pid = fork();
		
		if(pid == 0){
		
			char * m_ptr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, proc_cnt * PAGE_SIZE);
			
			if(m_ptr == MAP_FAILED){
				
				perror("Eroare la maparea unei pagini in memorie");
				return errno;
			}
			
			decrypt(m_ptr);
			
			return 0;
		}
	}
	
	// astept toti copiii
	while(wait(NULL) > 0);
	
	if(NOTIFICATIONS)
		printf("Decriptare finalizata\n");
	
	return 0;
}

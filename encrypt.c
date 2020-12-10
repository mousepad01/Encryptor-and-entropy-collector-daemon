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
#include <sys/socket.h>
#include <sys/un.h>

// pentru debug si afisari in consola
const int NOTIFICATIONS = 1;

size_t WORD_SIZE = sizeof(unsigned long);
size_t PAGE_SIZE;

int WORD_BIT_SIZE = 8 * sizeof(unsigned long);

const char * RANDOM_SOURCE = "/home/osboxes/proiect_so/sock_file";

const char * KEY_FILE = "/home/osboxes/proiect_so/keyfile.txt";

struct key{
	
	char * perm;
} KEY;

void itoa(int to_convert, char * buf){
 
	if(to_convert == 0){
 
		buf[0] = '0';
		buf[1] = '\0';
		return;
	}
 
	int dim = 0;
 
	while(to_convert > 0){
 
		buf[dim] = (to_convert % 10) + '0';
 
		to_convert /= 10;
		dim += 1;
	}
 
	char aux;
 
	for(int i = 0; i < dim / 2; i++){
 
		aux = buf[i];
		buf[i] = buf[dim - i - 1];
		buf[dim - i - 1] = aux;
	}
 
	buf[dim] = '\0';
}

unsigned char * get_rand(int req_b_size){

	struct sockaddr_un server_address;
	
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	
	server_address.sun_family = AF_UNIX;
	strcpy(server_address.sun_path, RANDOM_SOURCE);
	
	connect(sock, (struct sockaddr *) &server_address, sizeof(struct sockaddr_un));
	
	char * req_b_size_str = malloc(10);
	itoa(req_b_size, req_b_size_str);
	
	printf("%s ", req_b_size_str);
	
	int header = strlen(req_b_size_str);

	write(sock, &header, 4);
	write(sock, req_b_size_str, header);	
		
	// numarul de bytes primiti va fi cel mult egal cu nr de bytes ceruti
	unsigned char * requested_b = malloc(req_b_size + 1);
	
	int stream_size = 0;
	read(sock, &stream_size, 4);
	
	printf("%d ", stream_size);
	
	if(stream_size){
	
		read(sock, requested_b, stream_size);
		requested_b[stream_size] = '\0';
		
		//printf("%lu ", strlen(requested_b));
		
		//printf("%s STOP\n", requested_b);
	}
	else{
		// voi pune o valoare pseudorandom
		requested_b[0] = '5';
		requested_b[1] = '\0';
	}	
	
	close(sock);
	
	return requested_b;
}	

void keygen(){

	/* cheia va fi structurata in felul urmator: 
	 * o structura cu un array de 64 de char uri care alcatuieste permutarea aplicata
	 * (fiecare dintre acele char uri va avea valoarea in intervalul [1, 64] si va fi unica
	 */
	
	// plec de la permutarea identica

	KEY.perm = malloc(WORD_BIT_SIZE);
	 
	for(int i = 0; i < WORD_BIT_SIZE; i++)
		KEY.perm[i] = i;
		
	int swapaux;
		
	// aplic inversiuni random
	for(int inv = 0; inv < WORD_BIT_SIZE * 4; inv++){
	
		unsigned char * aux_i = get_rand(1);
		unsigned char * aux_j = get_rand(1);
		printf("%d ", aux_i[0] % WORD_BIT_SIZE);
		printf("%d\n", aux_j[0] % WORD_BIT_SIZE);
		unsigned char i = aux_i[0] % WORD_BIT_SIZE;
		unsigned char j = aux_j[0] % WORD_BIT_SIZE;
		
		//char i = getrand() % WORD_BIT_SIZE;
		//char j = getrand() % WORD_BIT_SIZE;
		
		swapaux = KEY.perm[i];
		KEY.perm[i] = KEY.perm[j];
		KEY.perm[j] = swapaux;
		
	}	
	
	int key_file_descriptor = open(KEY_FILE, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
		
	write(key_file_descriptor, KEY.perm, WORD_BIT_SIZE);	
	
	if(NOTIFICATIONS){
	
		for(int i = 0; i < WORD_BIT_SIZE; i++)
			printf("%d ", KEY.perm[i]);
			
		printf("\n");
	}
}

void encrypt(unsigned long * m_ptr){
	
	if(NOTIFICATIONS)
		printf("Procesul copil %d cripteaza..\n", getpid());
		
	for(int w_count = 0; w_count < PAGE_SIZE / WORD_SIZE; w_count++){
		
		unsigned long current_word = m_ptr[w_count];
		unsigned long encrypted_word = 0;
		
		for(int i = 0; i < WORD_BIT_SIZE; i++){
			
			encrypted_word |= ((current_word >> i) & 1) << KEY.perm[i];
		}
		
		m_ptr[w_count] = encrypted_word;
	}	
}

int main(int argc, char * argv[]){

	unlink(KEY_FILE);
	
	PAGE_SIZE = getpagesize();
	
	// deschiderea fisierului si preluarea file descriptor ului

	char * filename = argv[1];
	
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
		
	// generarea cheii si retinerea in fisier
	
	keygen();
	
	// fiecare proces cripteaza cate o pagina din memorie, iar fiecare proces va cripta pe rand
	// cate un cuvant de dimensiune WORD_SIZE
	
	for(int proc_cnt = 0; proc_cnt < n_pages; proc_cnt++){
		
		fflush(NULL); // pentru a evita afisari multiple din cauza copierii buffer ului de afisare in consola
		
		pid_t pid = fork();
		
		if(pid == 0){
		
			unsigned long * m_ptr = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, file_descriptor, proc_cnt * PAGE_SIZE);
			
			if(m_ptr == MAP_FAILED){
				
				perror("Eroare la maparea unei pagini in memorie");
				return errno;
			}
			
			encrypt(m_ptr);
			
			return 0;
		}
	}
	
	// astept toti copiii
	while(wait(NULL) > 0);
	
	if(NOTIFICATIONS)
		printf("Criptare finalizata\n");
	
	return 0;
}

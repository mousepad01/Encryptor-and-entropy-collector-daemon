#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <linux/input.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

/*
struct input_event {
	struct timeval time;
	__u16 type;
	__u16 code;
	__s32 value;
};
*/

const int NOTIFICATIONS = 1;

struct collected{
	
	int * val;
	int * time;
	
} collected_entropy;

const int COL_MAX_LEN = 32;

// aplic modulo TIME_PRECISION cand extrag momentul curent al evenimentului exprimat in microsecunde
const int TIME_PRECISION = 1000;

// pentru dimensiunea array urilor de informatie colectata in collected_entropy
int col_len = 0;

// mutex pentru a modifica col_len
pthread_mutex_t collect_mtx;

// mutex pentru a scrie / extrage din entropy_pool
pthread_mutex_t buffer_mtx;

// mutex pentru a scrie / extrage din fisierul de entropie
pthread_mutex_t file_mtx;

const char * KEYBOARD_EVENT_PATH = "/dev/input/event2";
const char * MOUSE_EVENT_PATH = "/dev/input/event4";

const size_t EVENT_SIZE = sizeof(struct input_event);

// combinatia de taste pentru a iesi din program
const int EXIT_CODE[2] = {29, 16};

// decalaj intre scroll uri inregistrate (in secunde)
const long int SCROLL_DECAL = 1;

// decalaj pentru thread ul de shuffle a bitilor colectati (in microsecunde)
const int CHECK_DELAY_USEC = 10000;

// dimensiunea unui element / unei unitati "random" generata
const int ELEMENT_SIZE = sizeof(unsigned short);

const int POOL_SIZE = 8192; // in bytes
//const int POOL_SIZE = 128; // pentru teste

int pool_index = 0; // ma voi folosi de acest indice pentru a retine capul stivei simulate

const char * RESERVE_POOL_FILE = "/home/osboxes/proiect_so/entropy_deposit.txt";

char * entropy_pool;

const char * SOCKET_ADDRESS = "/home/osboxes/proiect_so/sock_file";

const int SERVER_BUF_SIZE = 128; // in bytes

// -------------------------------------------------------------------------------------------

unsigned short ROTL(unsigned short x, int n){

	unsigned short k = (unsigned short)(n & (8 * ELEMENT_SIZE - 1));

	return (x << k) | (x >> (8 * ELEMENT_SIZE - k));
}

void * key_watch(void * arg){

	struct input_event key_event;

	int k_ev_descriptor = open(KEYBOARD_EVENT_PATH, O_RDONLY);
	
	// variabila auxiliara pentru a verifica daca a fost apasata o combinatie de 2 taste - pentru a opri thread ul
	int last_pressed_code = -1;

	while(1){
		
		read(k_ev_descriptor, &key_event, EVENT_SIZE);
		
		if(key_event.type == EV_KEY && key_event.value == 0){
		
			// pentru a iesi din thread, se apasa combinatia de taste din EXIT_CODE - dar se termina de apasat fiecare tasta 
			// inainte de apasarea urmatoarei taste : am implementat asa pentru a simplifica codul 
			// (decat sa fi verificat separat apasarea consecutiva a tastelor si key_event.value == 1 la ambele apasari)
			
			if(key_event.code == EXIT_CODE[1] && last_pressed_code == EXIT_CODE[0])
				return NULL;
			
			last_pressed_code = key_event.code;
			
			pthread_mutex_lock(&collect_mtx);
			
			col_len = (col_len + 1) % COL_MAX_LEN;
			
			collected_entropy.val[col_len] = key_event.code;
			collected_entropy.time[col_len] = key_event.time.tv_usec % TIME_PRECISION;
			
			if(NOTIFICATIONS)
				printf("key press %d\n", key_event.code);
			
			pthread_mutex_unlock(&collect_mtx);
		}
	}
}

void * mouse_watch(void * arg){

	struct input_event mouse_event;

	int m_ev_descriptor = open(MOUSE_EVENT_PATH, O_RDONLY);
	
	// pentru a retine cand s-a executat ultimul scroll
	long int last_scroll = -SCROLL_DECAL;

	while(1){
		
		read(m_ev_descriptor, &mouse_event, EVENT_SIZE);
		
		if(mouse_event.type == EV_KEY && mouse_event.value == 0){
		
			pthread_mutex_lock(&collect_mtx);
			
			col_len += 1;
			
			collected_entropy.val[col_len] = mouse_event.code;
			collected_entropy.time[col_len] = mouse_event.time.tv_usec;
			
			if(NOTIFICATIONS)
				printf("click %d\n", mouse_event.code);
			
			pthread_mutex_unlock(&collect_mtx);
		
		}
		else if(mouse_event.type == EV_REL && mouse_event.code == REL_WHEEL && mouse_event.time.tv_sec - last_scroll > SCROLL_DECAL){
		
			last_scroll = mouse_event.time.tv_sec;
			
			pthread_mutex_lock(&collect_mtx);
			
			col_len += 1;
			
			collected_entropy.val[col_len] = mouse_event.code * mouse_event.value;
			collected_entropy.time[col_len] = mouse_event.time.tv_usec;
			
			if(NOTIFICATIONS)
				printf("scroll %d\n", mouse_event.code * mouse_event.value);
			
			pthread_mutex_unlock(&collect_mtx);
		}
	}
}

void * mix(void * arg){

	int go_proc = 0;
	unsigned short * random_seq = malloc(COL_MAX_LEN * ELEMENT_SIZE);
	
	entropy_pool = malloc(POOL_SIZE);
	
	// instructiune necesara pentru cazul cand functia nu contine un cancellation point 
	// (setarea iplicita este PTHREAD_CANCEL_DEFERRED)
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

 	while(1){
 	
 		usleep(CHECK_DELAY_USEC);
 		
 		pthread_mutex_lock(&collect_mtx);
 			
 		if(col_len == COL_MAX_LEN - 1){
 		
 			col_len = 0;
 			
 			for(int i = 0; i < COL_MAX_LEN; i++)
 				random_seq[i] = collected_entropy.val[i] ^ collected_entropy.time[i];
 				
 			go_proc = 1;
 		}	
 			
 		pthread_mutex_unlock(&collect_mtx);
 		
 		if(go_proc){
 		
	 		// restul procesarii o fac in afara blocarii mutexului, intrucat nu este necesar sa blochez restul thread urilor in continuare
	 		
	 		// procesarea consta in cate o runda de RC5 pentru campurile i, i + 1, folosind pe post de "cheie" campurile i + 2, i + 3
	 		// am aplicat aceasta varianta, pentru a incerca sa ingreunez un reverse din valorile random finale 
	 		// inapoi in valorile I/O colectate si in timpii respectivi
	 		// daca inversarea ar avea loc, ar putea dezvalui informatii despre tastele apasate sau o posibila periodicitate a unor operatii I/O
	 		// si in plus ar putea ajuta la prezicerea urmatoarelor valori generate (bazate tot pe operatiile I/O efectuate pana acum si frecventa lor)
	 		
	 		for(int i = 0; i < COL_MAX_LEN; i += 4){
	 			
	 			random_seq[i] = ROTL(random_seq[i] ^ random_seq[i + 1], random_seq[i + 1]) + random_seq[i + 2];
	 			random_seq[i + 1] = ROTL(random_seq[i] ^ random_seq[i + 1], random_seq[i]) + random_seq[i + 3]; 
	 		}
	 		
	 		// selectarea elementelor dorite 
	 		
	 		unsigned short * filtered_rand_seq = malloc(ELEMENT_SIZE * COL_MAX_LEN / 2);
	 		
	 		int selected = 0;
	 		
	 		for(int i = 0; i < COL_MAX_LEN; i += 4){
	 			
	 			filtered_rand_seq[selected] = random_seq[i];
	 			filtered_rand_seq[selected + 1] = random_seq[i + 1];
	 			
	 			selected += 2;
	 		}
	 		
	 		// cantitatea de random este divizata in unitati de lungime ELEMENT_SIZE 
	 		// (o dimensiune optima, data de modul cum informatia este colectata - codurile tastelor apasate/ a mouse ului xor-ate cu numarul de 
	 		// milisecunde in momentul apasarii, rezultat apoi trunchiat pentru a nu aparea o periodicitate din cauza cifrelor de ordin cel mai mare a
	 		// momentelor de timp, ce ar aparea de exemplu cand cineva apasa multe taste rapid si consecutiv)
	 		// 
	 		// pentru convenienta divizarii ulterioare a acestei cantitati
	 		// voi stoca aceste unitati de dimensiune ELEMENT_SIZE intr-un string (cu dimensiunea elementelor 1 byte)
	 		// de aceea voi copia pe rand byte cu byte din filtered_rand_seq in entropy_pool
	 		
	 		// in momentul in care se atinge POOL_SIZE, se va deschide fisierul RESERVE_FILE in care se vor introduce bytii suplimentari
	 		// (pentru a nu fi irositi)
	 		// acest fisier va fi utilizat DOAR cand apare o cerere de byti care depaseste marimea stivei entropy_pool
	 		
	 		// am aplicat varianta ce combina un buffer cu utilizarea unui fisier
	 		// pentru a face un compromis intre rapiditatea extragerii cantitatii dorite, si dimensiunea alocata in memorie pentru acest lucru
	 		
	 		pthread_mutex_lock(&buffer_mtx);
	 		
	 		if(NOTIFICATIONS){
		 		for(int i = 0; i < selected; i++)
		 			printf("%u ", filtered_rand_seq[i]);
		 		printf("\n");
	 		}
	 		
	 		int i, b;	
	 		for(i = 0; i < selected && pool_index < POOL_SIZE; i++)
	 			for(b = 0; b < ELEMENT_SIZE && pool_index < POOL_SIZE; b++){
	 				
	 				entropy_pool[pool_index] = filtered_rand_seq[i] & 255;
	 				
	 				filtered_rand_seq[i] >>= 8;
	 				pool_index += 1;
	 			}
	 			
	 		pthread_mutex_unlock(&buffer_mtx);
	 			
	 		pthread_mutex_lock(&file_mtx);
	 			
	 		int depo_file_descriptor = open(RESERVE_POOL_FILE, O_WRONLY | O_APPEND | O_CREAT, S_IRWXU | S_IRWXO);
	 		
	 		// ignor ultimul element nefolosit / partial folosit din filtered_rand_seq, pentru simplitatea implementarii
	 		if(pool_index == POOL_SIZE && (selected - 1) - (i + 1) + 1 > 0){
	 			
	 			write(depo_file_descriptor, filtered_rand_seq + (i + 1), ELEMENT_SIZE * (selected - i - 1));	
	 		}
	 		
	 		close(depo_file_descriptor);
	 		
	 		pthread_mutex_unlock(&file_mtx);
	 		
	 		go_proc = 0;
 		}
 	}
}

void * server(void * arg){

	// in cazul cand fisierul a ramas acolo dintr-o sesiune anterioara, terminata cu eroare
	unlink(SOCKET_ADDRESS); 

	//pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	
	struct sockaddr_un server_spec;
	
	server_spec.sun_family = AF_UNIX;
	strcpy(server_spec.sun_path, SOCKET_ADDRESS);
	
	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	
	bind(sock, (struct sockaddr *) &server_spec, sizeof(struct sockaddr_un));
	
	listen(sock, 5);
	
	char * recv_buffer = malloc(SERVER_BUF_SIZE);
	
	int stream_size; // pentru a stoca dimensiunea pachetului ce urmeaza sa fie primit
	
	int active; // pentru a verifica starea conexiunii
	
	if(NOTIFICATIONS)
		printf("start listening..\n");
	
	while(1){
		
		int pair_sock = accept(sock, NULL, NULL);
		
		if(NOTIFICATIONS)
			printf("accepted connection\n");
		
		while(1){
			printf("here\n");
			active = read(pair_sock, &stream_size, 4);
			printf("here2\n");
			if(active){
				printf("stream size %d\n", stream_size);
				read(pair_sock, recv_buffer, stream_size);
				
				recv_buffer[stream_size] = '\0'; // mesajele primite au dimensiune diferita, trebuie sa termin string ul unde trebuie
				
				int requested_return_size = atoi(recv_buffer);
				
				if(requested_return_size == 0)
					write(pair_sock, &requested_return_size, 4);
				
				printf("requested_return_size %d | pool_index %d\n", requested_return_size, pool_index);
				
				// verific daca extrag din buffer sau din fisier
				// (precizare: voi putea extrage doar primii pool_index - 1 byti, pentru usurinta implementarii)
				
				char * to_return;
				
				if(requested_return_size >= pool_index){
					printf("first\n");
					// blochez fisierul pentru a nu avea probleme cand trunchez/citesc 
					pthread_mutex_lock(&file_mtx);
					
					int depo_file_descriptor = open(RESERVE_POOL_FILE, O_RDWR | O_CREAT, S_IRWXU | S_IRWXO);
					
					struct stat st;
					fstat(depo_file_descriptor, &st);
					
					off_t file_size = st.st_size;
					printf("file size %lu\n", file_size);
					// daca tot trebuie sa deschid fisierul, verific daca pot prelua byti doar din el, fara sa mai iau si din buffer
					if(file_size > requested_return_size){
						printf("f1\n");
						to_return = malloc(requested_return_size);
						
						lseek(depo_file_descriptor, file_size - requested_return_size, SEEK_SET);
						
						read(depo_file_descriptor, to_return, requested_return_size);
						
						close(depo_file_descriptor);
						
						truncate(RESERVE_POOL_FILE, file_size - requested_return_size);
						
						// trimiterea prin socket
						write(pair_sock, &requested_return_size, 4);
						write(pair_sock, to_return, requested_return_size);
					}
					else{	
						printf("f2\n");
						pthread_mutex_lock(&buffer_mtx);
						
						int get_from_buffer_size = requested_return_size - file_size;
						
						if(get_from_buffer_size >= pool_index){
							
							get_from_buffer_size = pool_index - 1;
							
							// aloc in memorie spatiu exact maximul cantitatii de returnat posibile
							to_return = malloc(file_size + get_from_buffer_size);
						}
						else
							to_return = malloc(requested_return_size);
						printf("get_from_buffer_size %d\n", get_from_buffer_size);
						// citesc tot ce pot din fisier
						read(depo_file_descriptor, to_return, file_size);
						
						close(depo_file_descriptor);
						
						// fisierul nu mai are in acest moment nimic in el, il sterg si va fi recreat cand va fi nevoie
						unlink(RESERVE_POOL_FILE);
						
						// citesc din buffer cat am nevoie
						
						int to_return_position = file_size;
						
						while(get_from_buffer_size){
							
							pool_index -= 1;
							get_from_buffer_size -= 1;
							
							to_return[to_return_position] = entropy_pool[pool_index];
							
							to_return_position += 1;
						}
						
						pthread_mutex_unlock(&buffer_mtx);
						
						// trimiterea prin socket
						printf("ok\n");
						// cazul cand si fisierul si buffer ul este gol / are doar un element, deci nu pot trimite nimic, semnalez asta
						if(to_return_position == 0)
							write(pair_sock, &to_return_position, 4);
						else{
							write(pair_sock, &to_return_position, 4);
							write(pair_sock, to_return, to_return_position);
						}
						
					}
					
					pthread_mutex_unlock(&file_mtx);
					
				}
				else{	
					printf("second\n");
					pthread_mutex_lock(&buffer_mtx);
					
					// cazul cand trimit doar din buffer
					
					to_return = malloc(requested_return_size);
					
					int to_return_position = 0;
					
					while(requested_return_size){
					
						pool_index -= 1;
						requested_return_size -= 1;
						
						to_return[to_return_position] = entropy_pool[pool_index];
						
						to_return_position += 1;
					}
					
					pthread_mutex_unlock(&buffer_mtx);
					
					// trimiterea prin socket (voi putea trimite sigur ceva, intrucat se verifica requested_return_size <= pool_index)
					
					write(pair_sock, &to_return_position, 4);
					write(pair_sock, to_return, to_return_position);
					
					printf("%d %s\n", to_return_position, to_return);
				}
			}
			else{
				if(NOTIFICATIONS)
					printf("closed connection\n");
					
				close(pair_sock);
				break;
			}
		}
	}
}

int main(){

	collected_entropy.val = malloc(COL_MAX_LEN * sizeof(int));
	collected_entropy.time = malloc(COL_MAX_LEN * sizeof(int));
	
	pthread_t mouse_watch_thread;
	pthread_t key_watch_thread;
	
	pthread_t mix_thread;
	
	pthread_t communication_thread;
	
	if(pthread_create(&mouse_watch_thread, NULL, mouse_watch, NULL)){
	
		perror("Eroare la lansarea unui thread");
		return errno;
	}
	
	if(pthread_create(&key_watch_thread, NULL, key_watch, NULL)){
	
		perror("Eroare la lansarea unui thread");
		return errno;
	}
	
	if(pthread_create(&mix_thread, NULL, mix, NULL)){
	
		perror("Eroare la lansarea unui thread");
		return errno;
	}
	
	if(pthread_create(&communication_thread, NULL, server, NULL)){
		
		perror("Eroare la lansarea unui thread");
		return errno;
	}
	
	// dupa ce thread ul ce monitorizeaza tastatura termina, se opreste fortat thread ul ce monitorizeaza mouse ul si cel care mixeaza datele
	
	if(pthread_join(key_watch_thread, NULL)){
	
		perror("Eroare la join ul unui thread");
		return errno;
	}
	
	if(pthread_cancel(mouse_watch_thread)){
		
		perror("Eroare la oprirea fortata a unui thread");
		return errno;
	}
	else if(pthread_join(mouse_watch_thread, NULL)){
		
		perror("Eroare la join ul unui thread");
		return errno;
	}
	
	if(pthread_cancel(mix_thread)){
		
		perror("Eroare la oprirea fortata a unui thread");
		return errno;
	}
	else if(pthread_join(mix_thread, NULL)){
	
		perror("Eroare la join ul unui thread");
		return errno;
	}
	
	if(pthread_cancel(communication_thread)){
		
		perror("Eroare la oprirea fortata a unui thread");
		return errno;
	}
	else if(pthread_join(communication_thread, NULL)){
	
		perror("Eroare la join ul unui thread");
		return errno;
	}
	
	unlink(SOCKET_ADDRESS);
	
	return 0;
}

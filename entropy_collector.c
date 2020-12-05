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

// conditie data de implementare: COL_MAX_LEN > 4 !!!
const int COL_MAX_LEN = 20;

// aplic modulo TIME_PRECISION cand extrag momentul curent al evenimentului exprimat in microsecunde
const int TIME_PRECISION = 1000;

// pentru dimensiunea array urilor de informatie colectata in collected_entropy
int col_len = 0;

// mutex pentru a modifica col_len
pthread_mutex_t mtx;

const char * KEYBOARD_EVENT_PATH = "/dev/input/event2";
const char * MOUSE_EVENT_PATH = "/dev/input/event4";

const size_t EVENT_SIZE = sizeof(struct input_event);

// combinatia de taste pentru a iesi din program
const int EXIT_CODE[2] = {29, 16};

// decalaj intre scroll uri inregistrate (in secunde)
const long int SCROLL_DECAL = 1;

// decalaj pentru thread ul de shuffle a bitilor colectati (in microsecunde)
const int CHECK_DELAY_USEC = 10000;

const int ELEMENT_SIZE = sizeof(unsigned short);

const char * POOL_FILE_NAME = "entropy_pool.txt";

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
			
			pthread_mutex_lock(&mtx);
			
			col_len = (col_len + 1) % COL_MAX_LEN;
			
			collected_entropy.val[col_len] = key_event.code;
			collected_entropy.time[col_len] = key_event.time.tv_usec % TIME_PRECISION;
			
			if(NOTIFICATIONS)
				printf("key press %d\n", key_event.code);
			
			pthread_mutex_unlock(&mtx);
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
		
			pthread_mutex_lock(&mtx);
			
			col_len += 1;
			
			collected_entropy.val[col_len] = mouse_event.code;
			collected_entropy.time[col_len] = mouse_event.time.tv_usec;
			
			if(NOTIFICATIONS)
				printf("click %d\n", mouse_event.code);
			
			pthread_mutex_unlock(&mtx);
		
		}
		else if(mouse_event.type == EV_REL && mouse_event.code == REL_WHEEL && mouse_event.time.tv_sec - last_scroll > SCROLL_DECAL){
		
			last_scroll = mouse_event.time.tv_sec;
			
			pthread_mutex_lock(&mtx);
			
			col_len += 1;
			
			collected_entropy.val[col_len] = mouse_event.code * mouse_event.value;
			collected_entropy.time[col_len] = mouse_event.time.tv_usec;
			
			if(NOTIFICATIONS)
				printf("scroll %d\n", mouse_event.code * mouse_event.value);
			
			pthread_mutex_unlock(&mtx);
		}
	}
}

void * mix(void * arg){

	int go_proc = 0;
	unsigned short * random_seq = malloc(COL_MAX_LEN * ELEMENT_SIZE);
	
	int pool_file_descriptor = open(POOL_FILE_NAME, O_WRONLY | O_APPEND | O_CREAT, S_IRWXU | S_IRWXO);
	
	// instructiune necesara pentru cazul cand functia nu contine un cancellation point 
	// (setarea iplicita este PTHREAD_CANCEL_DEFERRED)
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

 	while(1){
 	
 		usleep(CHECK_DELAY_USEC);
 		
 		pthread_mutex_lock(&mtx);
 			
 		if(col_len == COL_MAX_LEN - 1){
 		
 			col_len = 0;
 			
 			for(int i = 0; i < COL_MAX_LEN; i++)
 				random_seq[i] = collected_entropy.val[i] ^ collected_entropy.time[i];
 				
 			go_proc = 1;
 		}	
 			
 		pthread_mutex_unlock(&mtx);
 		
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
	 		
	 		// selectarea elementelor dorite si scrierea in fisier
	 		
	 		unsigned short * filtered_rand_seq = malloc(ELEMENT_SIZE * COL_MAX_LEN / 2);
	 		
	 		int j = 0;
	 		
	 		for(int i = 0; i < COL_MAX_LEN; i += 4){
	 			
	 			filtered_rand_seq[j] = random_seq[i];
	 			filtered_rand_seq[j + 1] = random_seq[i + 1];
	 			
	 			j += 2;
	 		}
	 		
	 		write(pool_file_descriptor, filtered_rand_seq, ELEMENT_SIZE * j);
	 		
	 		go_proc = 0;
 		}
 	}
}

int main(){

	collected_entropy.val = malloc(COL_MAX_LEN * sizeof(int));
	collected_entropy.time = malloc(COL_MAX_LEN * sizeof(int));
	
	pthread_t mouse_watch_thread;
	pthread_t key_watch_thread;
	
	pthread_t mix_thread;
	
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
	
	return 0;
}

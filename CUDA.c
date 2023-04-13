#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>
#include <cuda.h>
#include "cuda_runtime.h"
#include "device_launch_parameters.h"
/*zegar, obecnie ustawiony na 5 minut*/
bool time_controller = false;
void* timerThread(void* arg) {
	sleep(60 * 5);
	time_controller = false;
	return NULL;
}

struct client_list {
	int i; 		/*numer wierzchołka*/
	int x, y; 	/*współrzędne wierzchołka na płaszczyźnie (Euklidesowej)*/
	int q;		/*zapotrzebowanie na towar*/
	int e;		/*początek okna, w którym dostawa ma być wykonana*/
	int l;		/*koniec okna, w którym dostawa ma być wykonana*/
	int d;		/*czas obsuługi*/
	double road0; /*depota*/
	struct client_list* next; /*następny klient*/
	struct client_list* prev; /*ostatni klient*/
};

struct track_list { /*cięzarówka*/
	struct client_list* customer;
	float cur_cost; /*obecnie osiągnięta cena*/
	struct track_list* next; /*następna ciężarówka*/
};

struct solution_list { /*lista rozwiązań*/
	struct track_list* start; 
	struct solution_list* next;
};

struct sorted_clients{/*lista posortowana odnośnie l_depot*/
	int l;
	struct client_list* customer;
};

__global__ void clients_push_back_kernel(struct client_list** head_reference, struct client_list** tail_reference, int i, int x, int y, int q, int e, int l, int d, double road_to_depot) {
    struct client_list* newclient = (struct client_list*) malloc(sizeof(struct client_list));
    newclient->i = i;
    newclient->x = x;
    newclient->y = y;
    newclient->q = q;
    newclient->e = e;
    newclient->l = l;
    newclient->d = d;
    newclient->road0 = road_to_depot;

    newclient->next = NULL;
    newclient->prev = *tail_reference;
    if (*tail_reference != NULL) (*tail_reference)->next = newclient;
    *tail_reference = newclient;
    if (*head_reference == NULL) *head_reference = newclient;
}

void clients_push_back(struct client_list** head_reference, struct client_list** tail_reference, int i, int x, int y, int q, int e, int l, int d, double road_to_depot) {
    clients_push_back_kernel<<<1,1>>>(head_reference, tail_reference, i, x, y, q, e, l, d, road_to_depot);
    cudaDeviceSynchronize();
}

__global__ void add_solution_kernel(struct solution_list** headRef, struct track_list* begin) {
    struct solution_list* cur = *headRef;
    struct solution_list* client = (struct solution_list*) malloc(sizeof(struct solution_list));
    client->start = begin;
    client->next = NULL;

    if (cur == NULL) {
        *headRef = client;
    }
    else {
        while (cur->next != NULL) {
            cur = cur->next;
        }
        cur->next = client;
    }
}

void add_solution(struct solution_list** headRef, struct track_list* begin) {
    add_solution_kernel<<<1,1>>>(headRef, begin);
    cudaDeviceSynchronize();
}

__global__ void add_track_kernel(struct track_list** pointer, struct client_list* customer, int cur_cost) {
    struct track_list* cur = *pointer;

    struct track_list* node = (struct track_list*) malloc(sizeof(struct track_list));
    node->customer = customer;
    node->cur_cost = cur_cost;
    node->next = NULL;

    if (cur == NULL) {
        *pointer = node;
    }
    else {
        while (cur->next != NULL) {
            cur = cur->next;
        }
        cur->next = node;
    }
}

void add_track(struct track_list** pointer, struct client_list* customer, int cur_cost) {
    add_track_kernel<<<1,1>>>(pointer, customer, cur_cost);
    cudaDeviceSynchronize();
}

__global__ void clean_clients_kernel(struct client_list** head_reference) {
    struct client_list* client;
    while (*head_reference != NULL) {
        client = *head_reference;
        *head_reference = (*head_reference)->next;
        free(client);
    }
}

void clean_clients(struct client_list** head_reference) {
    clean_clients_kernel<<<1,1>>>(head_reference);
    cudaDeviceSynchronize();
}

/*selection sort*/
void Selection_sort(struct sorted_clients tab[], int n) {
	unsigned long int i, j, min;
	struct sorted_clients tmp;
	for (i = 0; i < n; i++) {
		min = i;
		for (j = i + 1; j < n; j++) {
			if (tab[j].l < tab[min].l)
				min = j;
		}
		tmp = tab[i];
		tab[i] = tab[min];
		tab[min] = tmp;
	}
}
/*--------------------------------------------------------------------------------*/
/*--------------------------------------------------------------------------------*/
/*--------------------------------------------------------------------------------*/
int main(int argc, char** argv) {
	if (argc <= 2) {
		printf("Usage: %s input_file output_file\n", argv[0]);
		return 1;
	}

	
	struct timeval time_start, time_end;
	gettimeofday(&time_start, NULL);
	
	int Q = 0; /*pojemność ciężarówki*/
	int x_depot, y_depot, e_depot, l_depot; /*dane o depo*/
	int i = 0, x = 0, y = 0, q = 0, e = 0, l = 0, d = 0; /*dane bieżącego wierzcholka; x,y-współrzędne; q-żądanie; e- początek okna; l-termin; d-czas obsługi;*/
	struct client_list* head = NULL, * tail = NULL; /*wskaźnik dla customer list*/

	double current_cost = 0, total_cost = 0; /*obecny koszt i długość trasy*/
	int vehicle_count = 0; /*końcowa ilość tras*/
	double road = 0;
	/* reading input file */
	FILE* input_file = NULL;
	input_file = fopen(argv[1], "r");
	if (input_file == NULL) {
		perror("Error opening input file ");
		return 1;
	}

	time_controller = true;/*zmienna odpowiadająca za czas wykonania programu*/

	fscanf(input_file, "%*s VEHICLE NUMBER CAPACITY %*d %d", &Q);
	fscanf(input_file, " CUSTOMER\nCUST NO. XCOORD. YCOORD. DEMAND READY TIME DUE DATE SERVICE TIME 0 %d %d %*d %d %d %*d\n", &x_depot, &y_depot, &e_depot, &l_depot);
	/*reading data of depot^*/

	while (!feof(input_file) && vehicle_count != -1) {
		fscanf(input_file, " %d %d %d %d %d %d %d\n", &i, &x, &y, &q, &e, &l, &d); 
		road = sqrt((double)((x_depot-x)*(x_depot-x)+(y_depot-y)*(y_depot-y)));
		if ((q > Q) || (road > l) || ((road > e ? road : e) + d + road) > l_depot) {
			vehicle_count = -1;
		}
		clients_push_back(&head, &tail, i, x, y, q, e, l, d, road);
	}
	fclose(input_file);

	/*tworzenie dodatkowej tabeli endwindow i jej sortowanie */
	int cur_vertex = 0; /*iterator for the table*/
	struct client_list* client = NULL;
	client = head;
	struct sorted_clients* sorted_list_of_clients = (struct sorted_clients*)calloc(i, sizeof(struct sorted_clients));
	while (client != NULL) {
		sorted_list_of_clients[cur_vertex].l = client->l;
		sorted_list_of_clients[cur_vertex].customer = client;

		client = client->next;
		cur_vertex++;
	}

	Selection_sort(sorted_list_of_clients, i);
	
	/*część algorytmu heurystycznego, generowanie rozwiązania*/
	int prev_x = 0, prev_y = 0, track_q = 0; /*współrzędne ostatniego odwiedzonego klienta, aktualna pojemność ciężarówki*/
	double prev_road = 0; /*droga do depo od ostatnio odwiedzonego wierzchołka*/
	double road_next = 0;  /*droga do ewentualnego klienta*/
	struct solution_list* solution = NULL; /*lista przechowywanych rozwiązań*/
	struct track_list* current_track = NULL;   /* początek trasy*/
	client = NULL;

	while (head != NULL && time_controller && vehicle_count != -1) {
		cur_vertex = 0;                  
		prev_x = x_depot; /*koordynata poprzedniego wierzchołka X*/
		prev_y = y_depot; /*koordynata poprzedniego wierzchołka Y*/
		track_q = Q; /* obecna pojemność ciężarówki*/
		current_cost = (double)e_depot; /*obecny koszt*/
		current_track = NULL;           /*obecna ciężarówka*/
		prev_road = 0;                /*droga do depota*/

		vehicle_count++;
		while (cur_vertex < i && track_q>0 && time_controller) {
			while (cur_vertex < i && (double)sorted_list_of_clients[cur_vertex].l < current_cost) cur_vertex++;
			if (cur_vertex < i) {
				client = sorted_list_of_clients[cur_vertex].customer;
				road_next = sqrt((double)((prev_x - client->x) * (prev_x - client->x) + (prev_y - client->y) * (prev_y - client->y)));/*dystans pomiedzy klientami*/
				if ((client->q <= track_q) && (road_next + current_cost <= (double)(client->l)) && 
					((road_next + current_cost > client->e ? road_next + current_cost : client->e) + client->d + client->road0 < l_depot)) {
					prev_x = client->x;
					prev_y = client->y;
					prev_road = client->road0;
					track_q -= client->q;
					if (road_next + current_cost > (double)client->e) {
						current_cost += road_next + (double)client->d;
					}
					else current_cost = (double)(client->e + client->d);

					add_track(&current_track, client, current_cost);
					if ((client->prev) != NULL) {
						(client->prev)->next = client->next;
					}
					else head = client->next;
					if ((client->next) != NULL) {
						(client->next)->prev = client->prev;
					}
					else tail = client->prev;
					if (client->prev == NULL && client->next == NULL) head = NULL;
					sorted_list_of_clients[cur_vertex].l = -1; /*zapowiedzenie tego, że wierzchołek nie będzie odwiedzony ponownie*/
				}
			}
			cur_vertex++;
		}

		add_solution(&solution, current_track);
		total_cost += current_cost + prev_road - (double)e_depot;
	}
	/* kompletne rozwiązanie w przypadku zakończenia danego limitu czasu */
	if (!time_controller && head != NULL) {
		current_track = NULL;
		client = head;
		head = head->next;

		current_cost = (client->road0 > client->e ? client->road0 : client->e) + client->d + client->road0;
		total_cost += current_cost;
		add_track(&current_track, client, current_cost);
		/*free(client);s*/
		add_solution(&solution, current_track);
	}

	/*koniec*/
	FILE* output_file = NULL;
	output_file = fopen(argv[2], "w");
	if (output_file == NULL) {
		perror("Error opening output file ");
		return 1;
	}
	fprintf(output_file, "%d %.5f\n", vehicle_count, total_cost);

	current_track = NULL;
	struct solution_list* cur_out = NULL;
	struct track_list* last = NULL;
	if (vehicle_count != -1) {
		while (solution != NULL) {
			current_track = solution->start;
			while (current_track->next != NULL) {
				last = current_track;
				current_track = current_track->next;

				fprintf(output_file, "%d ", last->customer->i);
				free(last);
			}
			fprintf(output_file, "%d\n", current_track->customer->i);
			free(current_track);
			cur_out = solution;
			solution = solution->next;
			free(cur_out);
		}
	}
	fclose(output_file);
	clean_clients(&head);

	gettimeofday(&time_end, NULL);
	time_t secs = time_end.tv_sec - time_start.tv_sec;
	suseconds_t usecs;
	if (time_end.tv_usec < time_start.tv_usec) {
		secs -= 1;
		usecs = time_start.tv_usec - time_end.tv_usec;
	}
	else {
		usecs = time_end.tv_usec - time_start.tv_usec;
	}
	printf("%s\n", argv[1]);
	printf("Time\t\tRoutes\t\tCost\n");
	printf("%d.%.6d\t%d\t\t%.5f\n", (int)secs, (int)usecs, vehicle_count, total_cost);
	return 0;
}

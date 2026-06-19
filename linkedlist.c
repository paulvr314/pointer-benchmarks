#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "shuffle.h"
#define PAGE_SIZE 4096
#define NODE_ARRAY_SIZE 1020
struct node {
	struct node *next; 
	int arr[NODE_ARRAY_SIZE];
}__attribute__((aligned (PAGE_SIZE)));

int main(int argc,char* argv[])
{
        if (argc != 5){
                printf("Usage: ll <log base 2 of num_nodes> <cyclesPerNode> <randomize traversal> <num_iterations>\n");
                exit(1);
        }

        /* Get num_nodes */
        int num_nodes = atoi(argv[1]); 
       
		/* Set total memory consumption */
	    num_nodes = 1UL << num_nodes;
	
        
	    /* Get cyclesPerNode */
        int cyclesPerNode = atoi(argv[2]); 
    
	int randomize_traversal = atoi(argv[3]);
        int num_iterations = atoi(argv[4]);
        /* Allocate and initialize the LL */
        struct node **ll = (struct node **) malloc (sizeof(struct node *) * num_nodes);
        assert(ll != NULL);
	for(int i = 0; i < num_nodes; i++) {
		ll[i] = malloc(sizeof(struct node));
                assert(ll[i] != NULL);
		
	}

        /* Get the Random ordering */
        int *randArr = getShuffled( num_nodes , randomize_traversal);

        /* Shuffle the List */
	int first = 1;
        int num  = 0;
	struct timeval begin, end;
    	gettimeofday(&begin, 0);

        struct node *ll_head, *ll_curr;
        for (int i = 0; i < num_nodes; i++ ) {

		printf("Assigning %d\n", i);
                num = randArr[i];
                if(first) {
                        first = 0;
                        memset(ll[num], 0, PAGE_SIZE);
                        ll_head = ll[num];
                        ll_curr = ll_head;
                } else {
                        memset(ll[num], 0, PAGE_SIZE);
                        ll_curr->next = ll[num];
                        ll_curr = ll_curr->next;
                }

        }
	gettimeofday(&end, 0);
    	long CDur = end.tv_sec - begin.tv_sec;
        free(randArr);
	

	printf("Traversal\n");

    	/* Traverse the List */
    	gettimeofday(&begin, 0);
        // clearCounters();
	int a;
	// scanf("%d", &a);
	ll_curr->next = ll[num];
	ll_curr = ll[num];
	ll_curr->next = NULL;

        for (int i = 0; i < num_iterations; i++){
                printf("\n\nIteration %d\n\n", i);
                int count = 0;
                ll_curr = ll_head;

                while(ll_curr != NULL) {
                        printf("%lx\n", ll_curr);
                        ll_curr = ll_curr->next;
                        count++;
                        if(ll_curr != NULL) {
                                for (int i = 0; i < cyclesPerNode ; i++){
                                        ll_curr->arr[i % NODE_ARRAY_SIZE] = count;
                                }
                        } 
                }
        }

	gettimeofday(&end, 0);
    	long TDur = end.tv_sec - begin.tv_sec;
	printf("Done\n");
    return 0;	

}

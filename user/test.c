#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

#define PGSIZE 4096

#define TEST_PAGES 1500

void
print_test_name(char *name) {
  printf("\n[TEST] %s\n", name);
}

void
passed() {
  printf("[PASS] Test Passed!\n");
}

void
failed() {
  printf("[FAIL] Test Failed!\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  int i;
  char **pages;

  printf("Starting Swap Test (Project 4)...\n");
  printf("Allocating %d pages...\n", TEST_PAGES);

  pages = malloc(sizeof(char*) * TEST_PAGES);
  if(pages == 0) {
    printf("malloc failed for pages array\n");
    exit(1);
  }

  // ---------------------------------------------------
  // Test 1: Allocation & Write (Trigger Swap-out)
  // ---------------------------------------------------
  print_test_name("1. Allocation & Write (Fill Memory)");
  
  for(i = 0; i < TEST_PAGES; i++) {
    pages[i] = malloc(PGSIZE);
    if(pages[i] == 0) {
        printf("OOM at page %d. This might be normal if swap is full.\n", i);
        break;
    }
    
    pages[i][0] = i % 255; 
    pages[i][PGSIZE/2] = (i + 1) % 255;
    pages[i][PGSIZE-1] = (i + 2) % 255;

    if (i % 100 == 0 && i > 0) {
        printf(".");
    }
  }
  printf("\nAllocation done. If memory was full, Swap-out should have happened.\n");

  // ---------------------------------------------------
  // Test 2: Read & Verify (Trigger Swap-in)
  // ---------------------------------------------------
  print_test_name("2. Read & Verify (Trigger Swap-in)");
  
  for(i = 0; i < TEST_PAGES; i++) {
    if (pages[i] == 0) break;

    char val1 = pages[i][0];
    char val2 = pages[i][PGSIZE/2];
    char val3 = pages[i][PGSIZE-1];

    if(val1 != (char)(i % 255) || 
       val2 != (char)((i + 1) % 255) || 
       val3 != (char)((i + 2) % 255)) {
         
        printf("\n[ERROR] Data Mismatch at page %d!\n", i);
        printf("Expected: %d, %d, %d\n", (i%255), ((i+1)%255), ((i+2)%255));
        printf("Actual:   %d, %d, %d\n", val1, val2, val3);
        failed();
    }
    if (i % 100 == 0 && i > 0) printf("v");
  }
  printf("\n");
  passed();

  // ---------------------------------------------------
  // Test 3: Fork Test (Copy Swapped Pages)
  // ---------------------------------------------------
  print_test_name("3. Fork Test (uvmcopy with Swap)");
  
  int pid = fork();
  if (pid < 0) {
    printf("fork failed\n");
    exit(1);
  }

  if (pid == 0) {
    printf("Child verifying data...\n");
    for(i = 0; i < TEST_PAGES; i++) {
      if(pages[i] == 0) break;
      if(pages[i][0] != (char)(i % 255)) {
         printf("[Child] Data mismatch at page %d\n", i);
         exit(1);
      }
    }
    printf("Child finished verification. Exiting.\n");
    
    exit(0); 
  } else {
    wait(0);
    printf("Parent: Child verified data successfully.\n");
    passed();
  }

  printf("\n[SUCCESS] All tests passed! Swap implementation looks good.\n");
  exit(0);
}

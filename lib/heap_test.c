/*
 *	BIRD Library -- Universal Heap Macros Tests
 *
 *	(c) 2015 CZ.NIC z.s.p.o.
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#include "test/birdtest.h"
#include "sysdep/config.h"
#include "lib/heap.h"

#define MAX_NUM 1000
#define SPECIAL_KEY -3213

#define MY_CMP(x, y) ((x) < (y))

#define MY_HEAP_SWAP(heap,a,b,t)		\
    do {					\
      bt_debug("swap(%d %d) ", a, b);		\
      HEAP_SWAP(heap,a,b,t);			\
    } while(0)

static int num;
static int heap[MAX_NUM+1];

/*
 * A valid heap must follow these rules:
 *   - `num >= 0`
 *   - `heap[i] >= heap[i / 2]` for each `i` in `[2, num]`
 */
static int
is_heap_valid(int heap[], int num)
{
  int i;

  if (num < 0)
    return 0;

  for (i = 2; i <= num; i++)
    if (heap[i] < heap[i / 2])
      return 0;

  return 1;
}

static void
show_heap(void)
{
  int i;
  bt_debug("\n");
  bt_debug("numbers %d; ", num);
  for (i = 0; i <= num; i++)
    bt_debug("%d ", heap[i]);
  bt_debug(is_heap_valid(heap, num) ? "OK" : "NON-VALID HEAP!");
  bt_debug("\n");
}

static void
init_heap(void)
{
  int i;
  num = 0;
  heap[0] = SPECIAL_KEY;		/* heap[0] should be unused */
  for (i = 1; i <= MAX_NUM; i++)
    heap[i] = 0;
}

static int
t_heap_insert(void)
{
  int i;

  init_heap();

  for (i = MAX_NUM; i >= 1; i--)
  {
    bt_debug("ins %d at pos %d ", i, MAX_NUM - i);
    heap[MAX_NUM - i + 1] = i;
    HEAP_INSERT(heap, ++num, int, MY_CMP, MY_HEAP_SWAP);
    show_heap();
    bt_assert(is_heap_valid(heap, num));
  }

  return BT_SUCCESS;
}

static int
t_heap_increase_decrease(void)
{
  int i;

  t_heap_insert();

  for (i = 1; i <= MAX_NUM; i++)
  {
    if (i > heap[i])
    {
      bt_debug("inc %d ", i);
      heap[i] = i;
      HEAP_INCREASE(heap, num, int, MY_CMP, MY_HEAP_SWAP, i);
    }
    else if (i < heap[i])
    {
      bt_debug("dec %d ", i);
      heap[i] = i;
      HEAP_INCREASE(heap, num, int, MY_CMP, MY_HEAP_SWAP, i);
    }
    show_heap();
    bt_assert(is_heap_valid(heap, num));
  }

  return BT_SUCCESS;
}

static int
t_heap_delete(void)
{
  int i;

  t_heap_insert();

  for (i = 1; i <= num; i++)
  {
    bt_debug("del at pos %d ", i);
    HEAP_DELETE(heap, num, int, MY_CMP, MY_HEAP_SWAP, i);
    show_heap();
    bt_assert(is_heap_valid(heap, num));
  }

  return BT_SUCCESS;
}

static int
t_heap_0(void)
{
  init_heap();
  t_heap_insert();
  t_heap_increase_decrease();
  t_heap_delete();

  return (heap[0] == SPECIAL_KEY) ? BT_SUCCESS : BT_FAILURE;
}

static int
t_heap_insert_random(void)
{
  int i, j;
  int expected[MAX_NUM+1];

  init_heap();

  for (i = 1; i <= MAX_NUM; i++)
  {
    heap[i] = expected[i] = bt_rand_num();
    HEAP_INSERT(heap, ++num, int, MY_CMP, MY_HEAP_SWAP);
    show_heap();
    bt_assert(is_heap_valid(heap, num));
  }

  for (i = 1; i <= MAX_NUM; i++)
    for (j = 1; j <= MAX_NUM; j++)
      if(expected[i] == heap[j])
	break;
      else if (j == MAX_NUM)
      {
	show_heap();
	bt_abort_msg("Did not find a number %d in heap.", expected[i]);
      }

  return BT_SUCCESS;
}

int
main(int argc, char *argv[])
{
  bt_init(argc, argv);

  bt_test_suite(t_heap_insert, "Inserting a descending sequence of numbers (the worst case)");
  bt_test_suite(t_heap_insert_random, "Inserting pseudo-random numbers");
  bt_test_suite(t_heap_increase_decrease, "Increasing/Decreasing");
  bt_test_suite(t_heap_delete, "Deleting");
  bt_test_suite(t_heap_0, "Is a heap[0] really unused?");

  return bt_end();
}
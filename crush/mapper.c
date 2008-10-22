
#ifdef __KERNEL__
# include <linux/string.h>
# include <linux/slab.h>
# include <asm/bug.h>
# include <linux/kernel.h>
#else
# include <string.h>
# include <stdio.h>
# include <assert.h>
# define BUG_ON(x) assert(!(x))
#endif

#include "crush.h"
#include "hash.h"


/**
 * crush_find_rule - find a crush_rule id for a given pool, type, and size.
 * @map: the crush_map
 * @pool: the storage pool id (user defined)
 * @type: storage pool type (user defined)
 * @size: output set size
 */
int crush_find_rule(struct crush_map *map, int pool, int type, int size)
{
	int i;

	for (i = 0; i < map->max_rules; i++) {
		if (map->rules[i] &&
		    map->rules[i]->mask.pool == pool &&
		    map->rules[i]->mask.type == type &&
		    map->rules[i]->mask.min_size <= size &&
		    map->rules[i]->mask.max_size >= size)
			return i;
	}
	return -1;
}


/*
 * bucket choose methods
 *
 * For each bucket algorithm, we have a "choose" method that, given a
 * crush input @x and replica position (usually, position in output set) @r,
 * will produce an item in the bucket.
 */

/* uniform */
static int bucket_uniform_choose(struct crush_bucket_uniform *bucket,
				       int x, int r)
{
	unsigned o = crush_hash32_2(x, bucket->h.id) & 0xffff;
	unsigned p = bucket->primes[crush_hash32_2(bucket->h.id, x) %
				    bucket->h.size];
	unsigned s = (x + o + (r+1)*p) % bucket->h.size;
	/*printf("%d %d %d %d\n", x, o, r, p);*/
	return bucket->h.items[s];
}

/* list */
static int bucket_list_choose(struct crush_bucket_list *bucket,
				    int x, int r)
{
	int i;

	for (i = 0; i < bucket->h.size; i++) {
		__u64 w = crush_hash32_4(x, bucket->h.items[i], r,
					 bucket->h.id);
		w &= 0xffff;
		/*printf("%d x %d item %d weight %d sum_weight %d r %lld",
		  i, x, bucket->h.items[i], bucket->item_weights[i], bucket->sum_weights[i], w);*/
		w *= bucket->sum_weights[i];
		w = w >> 16;
		/*printf(" scaled %lld\n", w);*/
		if (w < bucket->item_weights[i])
			return bucket->h.items[i];
	}

	BUG_ON(1);
	return 0;
}


/* tree */
static int height(int n) {
	int h = 0;
	while ((n & 1) == 0) {
		h++;
		n = n >> 1;
	}
	return h;
}

static int left(int x) {
	int h = height(x);
	return x - (1 << (h-1));
}

static int right(int x) {
	int h = height(x);
	return x + (1 << (h-1));
}

static int terminal(int x) {
	return x & 1;
}

static int bucket_tree_choose(struct crush_bucket_tree *bucket,
				    int x, int r)
{
	int n, l;
	__u32 w;
	__u64 t;

	/* start at root */
	n = bucket->h.size >> 1;

	while (!terminal(n)) {
		/* pick point in [0, w) */
		w = bucket->node_weights[n];
		t = (__u64)crush_hash32_4(x, n, r, bucket->h.id) * (__u64)w;
		t = t >> 32;

		/* descend to the left or right? */
		l = left(n);
		if (t < bucket->node_weights[l])
			n = l;
		else
			n = right(n);
	}

	return bucket->h.items[n];
}


/* straw */

static int bucket_straw_choose(struct crush_bucket_straw *bucket,
				     int x, int r)
{
	int i;
	int high = 0;
	__u64 high_draw = 0;
	__u64 draw;

	for (i = 0; i < bucket->h.size; i++) {
		draw = crush_hash32_3(x, bucket->h.items[i], r);
		draw &= 0xffff;
		draw *= bucket->straws[i];
		if (i == 0 || draw > high_draw) {
			high = i;
			high_draw = draw;
		}
	}
	return bucket->h.items[high];
}

static int crush_bucket_choose(struct crush_bucket *in, int x, int r)
{
	switch (in->alg) {
	case CRUSH_BUCKET_UNIFORM:
		return bucket_uniform_choose((struct crush_bucket_uniform *)in,
					     x, r);
	case CRUSH_BUCKET_LIST:
		return bucket_list_choose((struct crush_bucket_list *)in, x, r);
	case CRUSH_BUCKET_TREE:
		return bucket_tree_choose((struct crush_bucket_tree *)in, x, r);
	case CRUSH_BUCKET_STRAW:
		return bucket_straw_choose((struct crush_bucket_straw *)in,
					   x, r);
	default:
		BUG_ON(1);
		return in->items[0];
	}
}


/*
 * true if device is marked "out" (failed, fully offloaded)
 * of the cluster
 */
static int is_out(struct crush_map *map, int item, int x)
{
	if (map->device_offload[item]) {
		if (map->device_offload[item] >= 0x10000)
			return 1;
		else if ((crush_hash32_2(x, item) & 0xffff) <
			 map->device_offload[item])
			return 1;
	}
	return 0;
}

/**
 * crush_choose - choose numrep distinct items of given type
 * @map: the crush_map
 * @bucket: the bucket we are choose an item from
 * @x: crush input value
 * @numrep: the number of items to choose
 * @type: the type of item to choose
 * @out: pointer to output vector
 * @outpos: our position in that vector
 * @firstn: true if choosing "first n" items, false if choosing "indep"
 * @recurse_to_leaf: true if we want one device under each item of given type
 * @out2: second output vector for leaf items (if @recurse_to_leaf)
 */
static int crush_choose(struct crush_map *map,
			struct crush_bucket *bucket,
			int x, int numrep, int type,
			int *out, int outpos,
			int firstn, int recurse_to_leaf,
			int *out2)
{
	int rep;
	int ftotal, flocal;
	int retry_descent, retry_bucket, skip_rep;
	struct crush_bucket *in = bucket;
	int r;
	int i;
	int item;
	int itemtype;
	int collide, reject;

	for (rep = outpos; rep < numrep; rep++) {
		/* keep trying until we get a non-out, non-colliding item */
		ftotal = 0;
		skip_rep = 0;
		do {
			retry_descent = 0;
			in = bucket;               /* initial bucket */

			/* choose through intervening buckets */
			flocal = 0;
			do {
				retry_bucket = 0;
				r = rep;
				if (in->alg == CRUSH_BUCKET_UNIFORM) {
					/* be careful */
					if (firstn || numrep >= in->size)
						/* r' = r + f_total */
						r += ftotal;
					else if (in->size % numrep == 0)
						/* r'=r+(n+1)*f_local */
						r += (numrep+1) * flocal;
					else
						/* r' = r + n*f_local */
						r += numrep * flocal;
				} else {
					if (firstn)
						/* r' = r + f_total */
						r += ftotal;
					else
						/* r' = r + n*f_local */
						r += numrep * flocal;
				}

				/* bucket choose */
				item = crush_bucket_choose(in, x, r);
				BUG_ON(item >= map->max_devices);

				/* desired type? */
				if (item < 0)
					itemtype = map->buckets[-1-item]->type;
				else
					itemtype = 0;

				/* keep going? */
				if (itemtype != type) {
					BUG_ON(item >= 0 ||
					       (-1-item) >= map->max_buckets);
					in = map->buckets[-1-item];
					continue;
				}

				/* collision? */
				collide = 0;
				for (i = 0; i < outpos; i++) {
					if (out[i] == item) {
						collide = 1;
						break;
					}
				}

				/* out? */
				if (itemtype == 0)
					reject = is_out(map, item, x);
				else
					reject = 0;

				if (recurse_to_leaf &&
				    !crush_choose(map, map->buckets[-1-item],
						  x, 1, 0,
						  out2+outpos, 0,
						  firstn, 0, NULL))
					reject = 1;

				if (reject || collide) {
					ftotal++;
					flocal++;

					if (collide && flocal < 3)
						/* retry locally a few times */
						retry_bucket = 1;
					else if (ftotal < 10)
						/* then retry descent */
						retry_descent = 1;
					else
						/* else give up */
						skip_rep = 1;
				}
			} while (retry_bucket);
		} while (retry_descent);

		if (skip_rep) continue;

		out[outpos] = item;
		outpos++;
	}

	return outpos;
}


/**
 * crush_do_rule - calculate a mapping with the given input and rule
 * @map: the crush_map
 * @ruleno: the rule id
 * @x: hash input
 * @result: pointer to result vector
 * @result_max: maximum result size
 * @force: force initial replica choice; -1 for none
 */
int crush_do_rule(struct crush_map *map,
		  int ruleno, int x, int *result, int result_max,
		  int force)
{
	int result_len;
	int force_context[CRUSH_MAX_DEPTH];
	int force_pos = -1;
	int a[CRUSH_MAX_SET];
	int b[CRUSH_MAX_SET];
	int c[CRUSH_MAX_SET];
	int recurse_to_leaf;
	int *w;
	int wsize = 0;
	int *o;
	int osize;
	int *tmp;
	struct crush_rule *rule;
	int step;
	int i,j;
	int numrep;

	BUG_ON(ruleno >= map->max_rules);
	rule = map->rules[ruleno];
	result_len = 0;
	w = a;
	o = b;

	/*
	 * determine hierarchical context of force, if any.  note
	 * that this may or may not correspond to the specific types
	 * referenced by the crush rule.
	 */
	if (force >= 0) {
		if (force >= map->max_devices ||
		    map->device_parents[force] == 0) {
			/*printf("CRUSH: forcefed device dne\n");*/
			return -1;  /* force fed device dne */
		}
		if (!is_out(map, force, x)) {
			while (1) {
				force_context[++force_pos] = force;
				/*printf("force_context[%d] = %d\n", force_pos, force);*/
				if (force >= 0)
					force = map->device_parents[force];
				else
					force = map->bucket_parents[-1-force];
				if (force == 0)
					break;
			}
		}
	}

	for (step = 0; step < rule->len; step++) {
		switch (rule->steps[step].op) {
		case CRUSH_RULE_TAKE:
			w[0] = rule->steps[step].arg1;
			if (force_pos >= 0) {
				BUG_ON(force_context[force_pos] != w[0]);
				force_pos--;
			}
			wsize = 1;
			break;

		case CRUSH_RULE_CHOOSE_FIRSTN:
		case CRUSH_RULE_CHOOSE_INDEP:
		case CRUSH_RULE_CHOOSE_LEAF_FIRSTN:
		case CRUSH_RULE_CHOOSE_LEAF_INDEP:
			BUG_ON(wsize == 0);

			/* reset output */
			osize = 0;

			recurse_to_leaf = rule->steps[step].op >=
				CRUSH_RULE_CHOOSE_LEAF_FIRSTN;
			for (i = 0; i < wsize; i++) {
				/*
				 * see CRUSH_N, CRUSH_N_MINUS macros.
				 * basically, numrep <= 0 means relative to
				 * the provided result_max
				 */
				numrep = rule->steps[step].arg1;
				if (numrep <= 0) {
					numrep += result_max;
					if (numrep <= 0)
						continue;
				}
				j = 0;
				if (osize == 0 && force_pos >= 0) {
					/* skip any intermediate types */
					while (force_pos &&
					       force_context[force_pos] < 0 &&
					       rule->steps[step].arg2 !=
					       map->buckets[-1-force_context[force_pos]]->type)
						force_pos--;
					o[osize] = force_context[force_pos];
					if (recurse_to_leaf)
						c[osize] = force_context[0];
					j++;
					force_pos--;
				}
				osize += crush_choose(map,
						      map->buckets[-1-w[i]],
						      x, numrep,
						      rule->steps[step].arg2,
						      o+osize, j,
						      rule->steps[step].op ==
						       CRUSH_RULE_CHOOSE_FIRSTN,
						      recurse_to_leaf, c+osize);
			}

			if (recurse_to_leaf)
				/* copy final _leaf_ values to output set */
				memcpy(o, c, osize*sizeof(*o));

			/* swap t and w arrays */
			tmp = o;
			o = w;
			w = tmp;
			wsize = osize;
			break;


		case CRUSH_RULE_EMIT:
			for (i=0; i<wsize && result_len < result_max; i++) {
				result[result_len] = w[i];
				result_len++;
			}
			wsize = 0;
			break;

		default:
			BUG_ON(1);
		}
	}

	return result_len;
}



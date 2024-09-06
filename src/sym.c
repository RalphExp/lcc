#include "c.h"
#include <stdio.h>

static char rcsid[] = "$Id$";

#define equalp(x) v.x == p->sym.u.c.v.x

struct table {
	int level;
	Table previous;
	struct entry {
		struct symbol sym;
		struct entry *link;
	} *buckets[256];
	Symbol all; /* ch3. point to the last variable in the currenct scope,
	and we can use up pointer inside struct symbol to get all the
	variables. */
};
#define HASHSIZE NELEMS(((Table)0)->buckets)
static struct table
	cns = { CONSTANTS },
	ext = { GLOBAL },
	ids = { GLOBAL },
	tys = { GLOBAL };
Table constants   = &cns;
Table externals   = &ext;    // for extern
Table identifiers = &ids;
Table globals     = &ids;
Table types       = &tys;    // for types, see types.c
Table labels;                // for labels
int level = GLOBAL;
static int tempid;
List loci, symbols;

Table newtable(int arena) {
	Table new;

	NEW0(new, arena);
	return new;
}

/* All dynamically allocated tables are discarded after compiling each
 function, so they are allocated in the FUNC arena. */
Table table(Table tp, int level) {
	Table new = newtable(FUNC);
	new->previous = tp;
	new->level = level;
	if (tp)
		new->all = tp->all;
	return new;
}

/* ch3: scan a table and apply a given function to all symbols at a given scope. */
void foreach(Table tp, int lev, void (*apply)(Symbol, void *), void *cl) {
	assert(tp);
	while (tp && tp->level > lev)
		tp = tp->previous;
	if (tp && tp->level == lev) {
		Symbol p;
		Coordinate sav;
		// save
		sav = src;
		for (p = tp->all; p && p->scope == lev; p = p->up) {
			src = p->src;
			(*apply)(p, cl);
		}
		// restore
		src = sav;
	}
}

void enterscope(void) {
	if (++level == LOCAL)
		tempid = 0;
}

/* ch3: At scope exit, 1eve1 is decremented, and the corresponding identifiers
 * and types tables are removed. */
void exitscope(void) {
	rmtypes(level);
	if (types->level == level)
		types = types->previous;
	if (identifiers->level == level) {
		if (Aflag >= 2) {
			int n = 0;
			Symbol p;
			for (p = identifiers->all; p && p->scope == level; p = p->up)
				if (++n > 127) {
					warning("more than 127 identifiers declared in a block\n");
					break;
				}
		}
		identifiers = identifiers->previous;
	}
	assert(level >= GLOBAL);
	--level;
}

/* ch3: install allocates a symbol for name and adds it to a given table
 * at a specific scope level, allocating a new table if necessary. It
 * returns a symbol pointer. */
Symbol install(const char *name, Table *tpp, int level, int arena) {
	Table tp = *tpp;
	struct entry *p;
	unsigned h = (unsigned long)name&(HASHSIZE-1);

	/* a zero value for level indicates that name should be
	 installed in *tpp. */
	assert(level == 0 || level >= tp->level);
	if (level > 0 && tp->level < level)
		tp = *tpp = table(tp, level);
	NEW0(p, arena);
	p->sym.name = (char *)name;
	p->sym.scope = level;
	p->sym.up = tp->all;
	tp->all = &p->sym;
	p->link = tp->buckets[h];
	tp->buckets[h] = p;
	return &p->sym;
}

Symbol relocate(const char *name, Table src, Table dst) {
	struct entry *p, **q;
	Symbol *r;
	unsigned h = (unsigned long)name&(HASHSIZE-1);

	for (q = &src->buckets[h]; *q; q = &(*q)->link)
		if (name == (*q)->sym.name)
			break;
	assert(*q);
	/*
	 Remove the entry from src's hash chain
	  and from its list of all symbols.
	*/
	p = *q;
	*q = (*q)->link;
	for (r = &src->all; *r && *r != &p->sym; r = &(*r)->up)
		;
	assert(*r == &p->sym);
	*r = p->sym.up;
	/*
	 Insert the entry into dst's hash chain
	  and into its list of all symbols.
	  Return the symbol-table entry.
	*/
	p->link = dst->buckets[h];
	dst->buckets[h] = p;
	p->sym.up = dst->all;
	dst->all = &p->sym;
	return &p->sym;
}

/* ch3: lookup searches a table for a name, it handles lookups where the
 * search key is the name field of a symbol. It returns a symbol pointer
 * if it succeeds and the null pointer otherwise. */
Symbol lookup(const char *name, Table tp) {
	struct entry *p;
	unsigned h = (unsigned long)name&(HASHSIZE-1);

	assert(tp);
	do
		for (p = tp->buckets[h]; p; p = p->link)
			if (name == p->sym.name)
				return &p->sym;
	while ((tp = tp->previous) != NULL);
	return NULL;
}

int genlabel(int n) {
	static int label = 1;

	label += n;
	return label - n;
}

/* ch3: findlabel takes a label number and returns the corresponding
 * label symbol, installing and initializing it, and announcing it to
 * the back end, if necessary.*/
Symbol findlabel(int lab) {
	struct entry *p;
	unsigned h = lab&(HASHSIZE-1);

	for (p = labels->buckets[h]; p; p = p->link)
		if (lab == p->sym.u.l.label)
			return &p->sym;
	NEW0(p, FUNC);
	p->sym.name = stringd(lab);
	p->sym.scope = LABELS;
	p->sym.up = labels->all;
	labels->all = &p->sym;
	p->link = labels->buckets[h];
	labels->buckets[h] = p;
	p->sym.generated = 1;
	p->sym.u.l.label = lab;
	(*IR->defsymbol)(&p->sym);
	return &p->sym;
}

/* ch3: constant searches the constant table for a given value of a given type,
 * installing it if necessary, and returns the symbol pointer. Constants
 * are never removed from the table.*/
Symbol constant(Type ty, Value v) {
	struct entry *p;
	unsigned h = v.u&(HASHSIZE-1);
	static union { int x; char endian; } little = { 1 };

	ty = unqual(ty);
	for (p = constants->buckets[h]; p; p = p->link)
		if (eqtype(ty, p->sym.type, 1)) {
			switch (ty->op) {
			case INT:      if (equalp(i)) return &p->sym; break;
			case UNSIGNED: if (equalp(u)) return &p->sym; break;
			case FLOAT:
				if (v.d == 0.0) {
					float z1 = v.d, z2 = p->sym.u.c.v.d;
					char *b1 = (char *)&z1, *b2 = (char *)&z2;
					if (z1 == z2
					&& (!little.endian && b1[0] == b2[0] // XXX: ???
					||   little.endian && b1[sizeof (z1)-1] == b2[sizeof (z2)-1]))
						return &p->sym;
				} else if (equalp(d))
					return &p->sym;
				break;
			case FUNCTION: if (equalp(g)) return &p->sym; break;
			case ARRAY:
			case POINTER:  if (equalp(p)) return &p->sym; break;
			default: assert(0);
			}
		}
	NEW0(p, PERM);
	p->sym.name = vtoa(ty, v);
	p->sym.scope = CONSTANTS;
	p->sym.type = ty;
	p->sym.sclass = STATIC;
	p->sym.u.c.v = v;
	p->link = constants->buckets[h];
	p->sym.up = constants->all;
	constants->all = &p->sym;
	constants->buckets[h] = p;
	if (ty->u.sym && !ty->u.sym->addressed)
		(*IR->defsymbol)(&p->sym);
	p->sym.defined = 1;
	return &p->sym;
}

Symbol intconst(int n) {
	Value v;

	v.i = n;
	return constant(inttype, v);
}

/* ch3: The front end generates local variables for many purposes. For example,
 * it generates static variables to hold out-of-line constants like strings and
 * jump tables for switch statements. It generates locals to pass and return
 * structures to functions and to hold the results of conditional expressions
 * and switch values. genident allocates and initializes a generated identifier
 * of a specific type, storage class, and scope. */
Symbol genident(int scls, Type ty, int lev) {
	Symbol p;

	NEW0(p, lev >= LOCAL ? FUNC : PERM);
	p->name = stringd(genlabel(1));
	p->scope = lev;
	p->sclass = scls;
	p->type = ty;
	p->generated = 1;
	if (lev == GLOBAL)
		(*IR->defsymbol)(p);
	return p;
}

Symbol temporary(int scls, Type ty) {
	Symbol p;

	NEW0(p, FUNC);
	p->name = stringd(++tempid);
	p->scope = level < LOCAL ? LOCAL : level;
	p->sclass = scls;
	p->type = ty;
	p->temporary = 1;
	p->generated = 1;
	return p;
}

/* ch3: Temporaries are another kind of generated variable, and are
 * distinguished by a lit temporary flag. */
Symbol newtemp(int sclass, int tc, int size) {
	Symbol p = temporary(sclass, btot(tc, size));

	(*IR->local)(p);
	p->defined = 1;
	return p;
}

Symbol allsymbols(Table tp) {
	return tp->all;
}

void locus(Table tp, Coordinate *cp) {
	loci    = append(cp, loci);
	symbols = append(allsymbols(tp), symbols);
}

void use(Symbol p, Coordinate src) {
	Coordinate *cp;

	NEW(cp, PERM);
	*cp = src;
	p->uses = append(cp, p->uses);
}

/* findtype - find type ty in identifiers */
Symbol findtype(Type ty) {
	Table tp = identifiers;
	int i;
	struct entry *p;

	assert(tp);
	do
		for (i = 0; i < HASHSIZE; i++)
			for (p = tp->buckets[i]; p; p = p->link)
				if (p->sym.type == ty && p->sym.sclass == TYPEDEF)
					return &p->sym;
	while ((tp = tp->previous) != NULL);
	return NULL;
}

/* mkstr - make a string constant */
Symbol mkstr(char *str) {
	Value v;
	Symbol p;

	v.p = str;
	p = constant(array(chartype, strlen(v.p) + 1, 0), v);
	if (p->u.c.loc == NULL)
		p->u.c.loc = genident(STATIC, p->type, GLOBAL);
	return p;
}

/* mksymbol - make a symbol for name, install in &globals if sclass==EXTERN */
Symbol mksymbol(int sclass, const char *name, Type ty) {
	Symbol p;

	if (sclass == EXTERN) {
		p = install(string(name), &globals, GLOBAL, PERM);
	} else {
		NEW0(p, PERM);
		p->name = string(name);
		p->scope = GLOBAL;
	}
	p->sclass = sclass;
	p->type = ty;
	(*IR->defsymbol)(p);
	p->defined = 1;
	return p;
}

/* vtoa - return string for the constant v of type ty */
char *vtoa(Type ty, Value v) {
	ty = unqual(ty);
	switch (ty->op) {
	case INT:      return stringd(v.i);
	case UNSIGNED: return stringf((v.u&~0x7FFF) ? "0x%X" : "%U", v.u);
	case FLOAT:    return stringf("%g", (double)v.d);
	case ARRAY:
		if (ty->type == chartype || ty->type == signedchar
		||  ty->type == unsignedchar)
			return v.p;
		return stringf("%p", v.p);
	case POINTER:  return stringf("%p", v.p);
	case FUNCTION: return stringf("%p", v.g);
	}
	assert(0); return NULL;
}

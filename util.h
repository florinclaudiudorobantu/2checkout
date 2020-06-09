/* Dorobantu Florin-Claudiu */

#ifndef UTIL_H_
#define UTIL_H_

// Error printing macro
#define ERR(call_description)				\
	do {									\
		fprintf(stderr, "(%s, %d): ",		\
			__FILE__, __LINE__);			\
		perror(call_description);			\
	} while (0)

// Print error (call ERR) and exit
#define DIE(assertion, call_description)	\
	do {									\
		if (assertion) {					\
			ERR(call_description);			\
			exit(EXIT_FAILURE);				\
		}									\
	} while (0)

#endif /* UTIL_H_ */

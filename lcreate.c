/*
 * Copyright (c) 2003 Regents of The University of Michigan.
 * All Rights Reserved.  See COPYRIGHT.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <openssl/evp.h>
#include <snet.h>

#include "applefile.h"
#include "radstat.h"
#include "base64.h"
#include "cksum.h"
#include "connect.h"
#include "argcargv.h"
#include "code.h"
#include "tls.h"
#include "largefile.h"

/*
 * STOR
 * C: STOR <path-decription> "\r\n"
 * S: 350 Storing file "\r\n"
 * C: <size> "\r\n"
 * C: <size bytes of file data>
 * C: ".\r\n"
 * S: 250 File stored "\r\n"
 */

void		(*logger)( char * ) = NULL;
int		verbose = 0;
int		dodots = 0;
int		cksum = 0;
int		quiet = 0;
int		linenum = 0;
int		force = 0;
extern char	*version;
extern char	*checksumlist;
extern struct timeval   timeout;   
const EVP_MD    *md;
SSL_CTX  	*ctx;

extern char             *ca, *cert, *privatekey;

    static void
v_logger( char *line )
{
    printf( "<<< %s\n", line );
    return;
}

    int
main( int argc, char **argv )
{
    int			c, err = 0, port = htons(6662), tac; 
    int			network = 1, len, rc, lnbf = 0;
    int			negative = 0, tran_only = 0;
    int			respcount = 0;
    extern int		optind;
    struct servent	*se;
    SNET          	*sn = NULL;
    char		type;
    char		*tname = NULL, *host = _RADMIND_HOST; 
    char		*p,*d_path = NULL, tline[ 2 * MAXPATHLEN ];
    char		pathdesc[ 2 * MAXPATHLEN ];
    char		**targv;
    char                cksumval[ SZ_BASE64_E( EVP_MAX_MD_SIZE ) ];
    extern char		*optarg;
    struct timeval	tv;
    FILE		*tran; 
    struct stat		st;
    struct applefileinfo	afinfo;
    off_t		size = 0;
    int                 authlevel = _RADMIND_AUTHLEVEL;
    int                 use_randfile = 0;
    int                 login = 0;
    char                *user = NULL;
    char                *password = NULL;

    while (( c = getopt( argc, argv, "c:Fh:ilnNp:qrt:TU:vVw:x:y:z:" ))
	    != EOF ) {
	switch( c ) {
        case 'c':
            OpenSSL_add_all_digests();
            md = EVP_get_digestbyname( optarg );
            if ( !md ) {
                fprintf( stderr, "%s: unsupported checksum\n", optarg );
                exit( 2 );
            }
            cksum = 1;
            break;

	case 'F':
	    force = 1;
	    break;

	case 'h':
	    host = optarg; 
	    break;

	case 'i':
	    setvbuf( stdout, ( char * )NULL, _IOLBF, 0 );
	    lnbf = 1;
	    break;

        case 'l':
            login = 1;
            break;

	case 'n':
	    network = 0;
	    break;

	case 'N':
	    negative = 1;
	    break;

	case 'p':
	    if (( port = htons( atoi( optarg ))) == 0 ) {
		if (( se = getservbyname( optarg, "tcp" )) == NULL ) {
		    fprintf( stderr, "%s: service unknown\n", optarg );
		    exit( 2 );
		}
		port = se->s_port;
	    }
	    break;

	case 'q':
	    quiet = 1;
	    break;
	case 'r':
	    use_randfile = 1;
	    break;

	case 't':
	    tname = optarg;
	    break;

	case 'T':
	    tran_only = 1;
	    break;

        case 'U':
            user = optarg;
            break;

	case 'v':
	    verbose = 1;
	    logger = v_logger;
	    if ( isatty( fileno( stdout ))) {
		dodots = 1;
	    }
	    break;

	case 'V':
	    printf( "%s\n", version );
	    printf( "%s\n", checksumlist );
	    exit( 0 );

        case 'w' :              /* authlevel 0:none, 1:serv, 2:client & serv */
            authlevel = atoi( optarg );
            if (( authlevel < 0 ) || ( authlevel > 2 )) {
                fprintf( stderr, "%s: invalid authorization level\n",
                        optarg );
                exit( 1 );
            }
            break;

        case 'x' :              /* ca file */
            ca = optarg;
            break;

        case 'y' :              /* cert file */
            cert = optarg;
            break;

        case 'z' :              /* private key */
            privatekey = optarg;
            break;

	case '?':
	    err++;
	    break;
	default:
	    err++;
	    break;
	}
    }

    if ( verbose && quiet ) {
	err++;
    }
    if ( verbose && lnbf ) {
	err++;
    }

    if ( err || ( argc - optind != 1 ))   {
	fprintf( stderr, "usage: lcreate [ -FlnNrTV ] [ -q | -v | -i ] " );
	fprintf( stderr, "[ -c checksum ] " );
	fprintf( stderr, "[ -h host ] [ -p port ] " );
	fprintf( stderr, "[ -t stored-name ] [ -U user ] " );
        fprintf( stderr, "[ -w authlevel ] [ -x ca-pem-file ] " );
        fprintf( stderr, "[ -y cert-pem-file] [ -z key-pem-file ] " );
	fprintf( stderr, "create-able-transcript\n" );
	exit( 2 );
    }

    if ( network ) {

	/*
	 * Pipelining creates an annoying problem: the server might
	 * have closed our connection a long time before we get around
	 * to reading an error.  In the meantime, we will do a lot
	 * of writing, which may cause us to be killed.
	 */
	if ( signal( SIGPIPE, SIG_IGN ) == SIG_ERR ) {
	    perror( "signal" );
	    exit( 2 );
	}

	if ( authlevel != 0 ) {
	    if ( tls_client_setup( use_randfile, authlevel, ca, cert, 
		    privatekey ) != 0 ) {
		/* error message printed in tls_setup */
		exit( 2 );
	    }
	}

	/* no name given on command line, so make a "default" name */
	if ( tname == NULL ) {
	    tname = argv[ optind ];
	    /* strip leading "/"s */
	    if (( p = strrchr( tname, '/' )) != NULL ) {
		tname = ++p;
	    }
	}

	if (( sn = connectsn( host, port )) == NULL ) {
	    exit( 2 );
	}

        if ( authlevel != 0 ) {
            if ( tls_client_start( sn, host, authlevel ) != 0 ) {
                /* error message printed in tls_cleint_starttls */
                exit( 2 );
            }
        }

        if ( login ) {
	    char		*line;

	    if ( authlevel < 1 ) {
		fprintf( stderr, "login requires TLS\n" );
		exit( 2 );
	    }
            if ( user == NULL ) {
                if (( user = getlogin()) == NULL ) {
		    perror( "getlogin" );
                    exit( 2 );
                } 
            }
            if ( password == NULL ) {
		printf( "user: %s\n", user );
                if (( password = getpass( "password:" )) == NULL ) {
                    fprintf( stderr, "Invalid null password\n" );
                    exit( 2 );
                }
		/* get the length of the password so we can zero it later */
		len = strlen( password );
            }
            if ( verbose ) printf( ">>> LOGIN %s %s\n", user, password );
            if ( snet_writef( sn, "LOGIN %s %s\n", user, password ) < 0 ) {
                fprintf( stderr, "login %s failed: 1-%s\n", user, 
                    strerror( errno ));
                exit( 2 );                       
            }                            
	    tv = timeout;
	    if (( line = snet_getline_multi( sn, logger, &tv )) == NULL ) {
		fprintf( stderr, "login %s failed: 2-%s\n", user,
		    strerror( errno ));
		exit( 2 );
	    }
	    if ( *line != '2' ) {
		fprintf( stderr, "%s\n", line );
		return( 1 );
	    }

	    /* clear the password from memory */
	    if ( len ) {
		memset( password, 0, len );
	    }
        }

	if ( cksum ) {
	    if ( do_cksum( argv[ optind ], cksumval ) < 0 ) {
		perror( tname );
		exit( 2 );
	    }
	}

	if ( snprintf( pathdesc, MAXPATHLEN * 2, "STOR TRANSCRIPT %s",
		tname ) > ( MAXPATHLEN * 2 ) - 1 ) {
	    fprintf( stderr, "STOR TRANSCRIPT %s: path description too long\n",
		tname );
	}

	/* Get transcript size */
	if ( stat( argv[ optind ], &st ) != 0 ) {
	    perror( argv[ optind ] );
	    exit( 2 );
	}

	respcount += 2;
	if (( rc = stor_file( sn, pathdesc, argv[ optind ], st.st_size,
		cksumval )) <  0 ) {
	    goto stor_failed;
	}

	if ( tran_only ) {	/* don't upload files */
	    goto done;
	}
    }

    if (( tran = fopen( argv[ optind ], "r" )) < 0 ) {
	perror( argv[ optind ] );
	exit( 2 );
    }

    while ( fgets( tline, MAXPATHLEN, tran ) != NULL ) {
	if ( network && respcount > 0 ) {
	    tv.tv_sec = 0;
	    tv.tv_usec = 0;
	    if ( stor_response( sn, &respcount, &tv ) < 0 ) {
		exit( 2 );
	    }
	}

	len = strlen( tline );
	if (( tline[ len - 1 ] ) != '\n' ) {
	    fprintf( stderr, "%s: line too long\n", tline );
	    exit( 2 );
	}
	linenum++;
	tac = argcargv( tline, &targv );

	/* skips blank lines and comments */
	if (( tac == 0 ) || ( *targv[ 0 ] == '#' )) {
	    continue;
	}

	if ( tac == 1 ) {
	    fprintf( stderr, "Appliable transcripts cannot be uploaded.\n" );
	    exit( 2 );
	}
	if ( *targv[ 0 ] == 'f' || *targv[ 0 ] == 'a' ) {
	    if ( tac != 8 ) {
		fprintf( stderr, "line %d: invalid transcript line\n",
			linenum );
		exit( 2 );
	    }

	    if (( d_path = decode( targv[ 1 ] )) == NULL ) {
		fprintf( stderr, "line %d: path too long\n", linenum );
		return( 1 );
	    } 

	    if ( !negative ) {
		/* Verify transcript line is correct */
		if ( radstat( d_path, &st, &type, &afinfo ) != 0 ) {
		    perror( d_path );
		    exit( 2 );
		}
		if ( *targv[ 0 ] != type ) {
		    fprintf( stderr, "line %d: file type wrong\n", linenum );
		    exit( 2 );
		}
	    }

	    if ( !network ) {
		if ( cksum ) {
		    if ( *targv[ 0 ] == 'f' ) {
			size = do_cksum( d_path, cksumval );
		    } else {
			/* apple file */
			size = do_acksum( d_path, cksumval, &afinfo );
		    }
		    if ( size < 0 ) {
			fprintf( stderr, "%s: %s\n", d_path, strerror( errno ));
			exit( 2 );
		    } else if ( size != strtoofft( targv[ 6 ], NULL, 10 )) {
			fprintf( stderr, "line %d: size in transcript does "
			    "not match size of file\n", linenum );
			exit( 2 );
		    }
		    if ( strcmp( cksumval, targv[ 7 ] ) != 0 ) {
			fprintf( stderr,
			    "line %d: checksum listed in transcript wrong\n",
			    linenum );
			return( -1 );
		    }
		}
		if ( access( d_path,  R_OK ) < 0 ) {
		    perror( d_path );
		    exit( 2 );
		}
	    } else {
		if ( snprintf( pathdesc, MAXPATHLEN * 2, "STOR FILE %s %s", 
			tname, targv[ 1 ] ) > ( MAXPATHLEN * 2 ) - 1 ) {
		    fprintf( stderr, "STOR FILE %s %s: path description too \
			long\n", tname, d_path );
		    exit( 2 );
		}

		if ( negative ) {
		    if ( *targv[ 0 ] == 'a' ) {
			rc = n_stor_applefile( sn, pathdesc, d_path );
		    } else {
			rc = n_stor_file( sn, pathdesc, d_path );
		    }
		    respcount += 2;
		    if ( rc < 0 ) {
			goto stor_failed;
		    }

		} else {
		    if ( *targv[ 0 ] == 'a' ) {
			rc = stor_applefile( sn, pathdesc, d_path,
			    strtoofft( targv[ 6 ], NULL, 10 ), targv[ 7 ],
			    &afinfo );
		    } else {
			rc = stor_file( sn, pathdesc, d_path, 
			    strtoofft( targv[ 6 ], NULL, 10 ), targv[ 7 ]); 
		    }
		    respcount += 2;
		    if ( rc < 0 ) {
			if ( dodots ) { putchar( (char)'\n' ); }
			goto stor_failed;
		    }
		}
	    }
	}
    }

done:
    if ( network ) {
	while ( respcount > 0 ) {
	    if ( stor_response( sn, &respcount, NULL ) < 0 ) {
		exit( 2 );
	    }
	}
	if (( closesn( sn )) != 0 ) {
	    fprintf( stderr, "cannot close sn\n" );
	    exit( 2 );
	}
    }

    exit( 0 );

stor_failed:
    while ( respcount > 0 ) {
	tv.tv_sec = 30;
	tv.tv_usec = 0;
	if ( stor_response( sn, &respcount, &tv ) < 0 ) {
	    exit( 2 );
	}
    }
    exit( 2 );
}

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>
#include <sha.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mkdev.h>
#include <sys/ddi.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>

#include "snet.h"
#include "argcargv.h"
#include "getstat.h"
#include "download.h"
#include "chksum.h"
#include "list.h"

void output( char* string);
int check( SNET *sn, char *type, char *path); 
int createspecial( SNET *sn, struct node *head );

void		(*logger)( char * ) = NULL;
struct timeval 	timeout = { 10 * 60, 0 };
int		linenum = 0;
int		chksum = 1;
int		verbose = 0;
int		network = 1;
char		*command = "command.K";

    void
output( char *string ) {
    printf( "<<< %s\n", string );
    return;
}

    int
createspecial( SNET *sn, struct node *head ) {
    int			fd;
    FILE		*fs;
    struct node 	*prev;
    char		pathdesc[ MAXPATHLEN * 2 ];
    char		*stats;

    if ( verbose ) printf( "\n*** Creating special.T\n" );

    /* Open file */
    if ( ( fd = open( "special.T", O_WRONLY | O_CREAT, 0666 ) ) < 0 ) {
	perror( "special.T" );
	return( -1 );
    }
    if ( ( fs = fdopen( fd, "w" ) ) == NULL ) {
	fprintf( stderr, "fdopen" );
	return( 1 );
    }

    do {
	sprintf( pathdesc, "SPECIAL %s", head->path);

	if ( verbose ) printf( "\n*** Statting %s\n", head->path );

	if ( ( stats = getstat( sn, (char *)&pathdesc) ) == NULL ) {
	    fprintf( stderr, "getstat\n" );
	    return( 1 );
	}

	if ( fputs( stats, fs) == EOF ) {
	    fprintf( stderr, "fputs" );
	    return( 1 );
	}
	if ( fputs( "\n", fs) == EOF ) {
	    fprintf( stderr, "fputs" );
	    return( 1 );
	}

	prev = head;
	head = head->next;

	free( prev->path );
	free( prev );
    } while ( head != NULL );

    if ( fclose( fs ) != 0 ) {
	fprintf( stderr, "flcose" );
	return( 1 );
    }

    return( 0 );
}

    int
check( SNET *sn, char* type, char *path)
{
    char	*schksum, *stats;
    char	**targv;
    char 	pathdesc[ 2 * MAXPATHLEN ];
    char        cchksum[ 29 ];
    int		error, tac;

    if ( verbose ) printf( "\n" );

    if ( path != NULL ) {
	sprintf( pathdesc, "%s %s", type, path);
    } else {
	sprintf( pathdesc, "%s", type );
	path = command;
    }

    if ( verbose ) printf( "*** Statting %s\n", path );

    stats = getstat( sn, (char *)&pathdesc);

    tac = acav_parse( NULL, stats, &targv );

    if ( tac != 8 ) {
	perror( "Incorrect number of arguments\n" );
	return( 1 );
    }

    if ( ( schksum = strdup( targv[ 7 ] ) ) == NULL ) {
	perror( "strdup" );
	return( 1 );
    }

    error = do_chksum( path, cchksum );

    if ( error == 2 ) {
	if ( verbose ) printf( "*** Downloading missing file: %s\n", path ); 
	if ( download( sn, pathdesc, path, schksum ) != 0 ) {
	    perror( "download" );
	    return( 1 );
	}
	return( 0 );
    } else if ( error == 1 ) {
	perror( "do_chksum" );
	return( 1 );
    }

    if ( verbose ) printf( "*** chskum " );

    if ( strcmp( schksum, cchksum) != 0 ) {
	if ( verbose ) printf( "mismatch on %s\n", path );
	if ( network ) {
	    if ( unlink( path ) != 0 ) {
		perror( "unlink" );
		return( 1 );
	    }
	    if ( verbose ) printf( "*** %s deleted\n", path );
	    if ( verbose ) printf( "*** Downloading %s\n", path ); 
	    if ( download( sn, pathdesc, path, schksum ) != 0 ) {
		perror( "download" );
		return( 1 );
	    }
	}
    } else {
	if ( verbose ) printf( "match\n" );
    }

    return( 0 );
}

    int
main( int argc, char **argv )
{
    int			i, c, s, port = htons( 6662 ), err = 0;
    int			len, tac;
    extern int          optind;
    char		*host = NULL, *line = NULL, *version = "1.0";
    char		*transcript = NULL;
    char                **targv;
    char                cline[ 2 * MAXPATHLEN ];
    struct servent	*se;
    struct hostent	*he;
    struct sockaddr_in	sin;
    SNET		*sn;
    struct timeval	tv;
    FILE		*f;
    struct node		*head = NULL;

    while ( ( c = getopt ( argc, argv, "c:K:np:T:Vv" ) ) != EOF ) {
	switch( c ) {
	case 'c':
	    if ( strcasecmp( optarg, "sha1" ) != 0 ) {
		perror( optarg );
		exit( 1 );
	    }
	    chksum = 1;
	    break;
	case 'p':
	    if ( ( port = htons ( atoi( optarg ) ) ) == 0 ) {
		if ( ( se = getservbyname( optarg, "tcp" ) ) == NULL ) {
		    fprintf( stderr, "%s: service unkown\n", optarg );
		    exit( 1 );
		}
		port = se->s_port;
	    }
	    break;
	case 'K':
	    command = optarg;
	    break;
	case 'n':
	    printf( "No download\n" );
	    network = 0;
	    break;
	case 'T':
	    transcript = optarg;
	    break;
	case 'V':
	    printf( "%s\n", version );
	    exit( 0 );
	case 'v':
	    verbose = 1;
	    logger = output;
	    break;
	case '?':
	    err++;
	    break;
	default:
	    err++;
	    break;
	}
    }

    if ( err || ( argc - optind != 1 ) ) {
	fprintf( stderr, "usage: ktcheck [ -nvV ] " );
	fprintf( stderr, "[ -c checksum ] [ -K command file ] " );
	fprintf( stderr, "[ -p port ] " );
	fprintf( stderr, "[-T transcript ] host\n" );
	exit( 1 );
    }
    host = argv[ optind ];

    /* Network connection */
    if ( ( he = gethostbyname( host ) ) == NULL ) {
	perror( host );
	exit( 1 );
    }

    for ( i = 0; he->h_addr_list[ i ] != NULL; i++ ) {
	if ( ( s = socket( PF_INET, SOCK_STREAM, NULL ) ) < 0 ) {
	    perror ( host );
	    exit( 1 );
	}
	memset( &sin, 0, sizeof( struct sockaddr_in ) );
	sin.sin_family = AF_INET;
	sin.sin_port = port;
	memcpy( &sin.sin_addr.s_addr, he->h_addr_list[ i ],
	    ( unsigned int)he->h_length );
	if ( verbose ) printf( "trying %s... ",
		inet_ntoa( *( struct in_addr *)he->h_addr_list[ i ] ) );
	if ( connect( s, ( struct sockaddr *)&sin,
		sizeof( struct sockaddr_in ) ) != 0 ) {
	    perror( "connect" );
	    (void)close( s );
	    continue;
	}
	if ( verbose ) printf( "success!\n" );

	if ( ( sn = snet_attach( s, 1024 * 1024 ) ) == NULL ) {
	    perror ( "snet_attach failed" );
	    continue;
	}

	tv.tv_sec = 10;
	tv.tv_usec = 0;
	if ( ( line = snet_getline_multi( sn, logger, &tv) ) == NULL ) {
	    perror( "snet_getline_multi" );
	    if ( snet_close( sn ) != 0 ) {
		perror ( "snet_close" );
	    }
	    continue;
	}

	if ( *line !='2' ) {
	    fprintf( stderr, "%s\n", line);
	    if ( snet_close( sn ) != 0 ) {
		perror ( "snet_close" );
	    }
	    continue;
	}
	break;
    }

    if ( he->h_addr_list[ i ] == NULL ) {
	perror( "connection failed" );
	exit( 1 );
    }

    if ( check( sn, "COMMAND", NULL ) != 0 ) { 
	perror( "check" );
	exit( 1 );
    }

    if ( ( f = fopen( command, "r" ) ) == NULL ) {
	perror( argv[ 1 ] );
	exit( 1 );
    }

    while ( fgets( cline, MAXPATHLEN, f ) != NULL ) {
	linenum++;

	len = strlen( cline );
	if (( cline[ len - 1 ] ) != '\n' ) {
	    fprintf( stderr, "%s: line too long\n", cline );
	    exit( 1 );
	}

	tac = acav_parse( NULL, cline, &targv );

	if ( ( tac == 0 ) || ( *targv[ 0 ] == '#' ) ) {
	    continue;
	}

	if ( tac != 2 ) {
	    fprintf( stderr, "invalid command line %d\n", linenum );
	    exit( 1 );
	}

	if ( *targv[ 0 ] == 's' ) {
	    insert_node( targv[ 1 ], &head);
	    continue;
	}
	    
	if ( check( sn, "TRANSCRIPT", targv[ tac - 1] ) != 0 ) {
	    perror( "check" );
	    exit( 1 );
	}
    }

    if ( head != NULL ) {
	createspecial( sn, head );
    }


    /* Close network connection */
    if ( snet_writef( sn, "QUIT\r\n" ) == NULL ) {
	perror( "snet_writef" );
	exit( 1 );
    }

    if ( ( line = snet_getline_multi( sn, logger, &tv ) ) == NULL ) {
	perror( "snet_getline_multi" );
	exit( 1 );
    }

    if ( *line != '2' ) {
	perror( line );
    }

    if ( snet_close( sn ) != 0 ) {
	perror( "snet_close" );
	exit( 1 );
    }

    exit( 0 );
}
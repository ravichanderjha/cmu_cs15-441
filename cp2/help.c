#include "help.h"

/******************************************************************************
* subroutine: init_pool                                                       *
* purpose:    setup the initial value for pool attributes                     *
* parameters: sock - the descriptor server using to listen client connections *
*             p    - pointer to pool instance                                 *
* return:     none                                                            *
******************************************************************************/
void init_pool (pool *p)
{
	// Initialize descriptors
	int i;
	p->maxi = -1;
	for (i=0; i< FD_SETSIZE; i++)
	p->clientfd[i] = -1;
	if(STATE.sock < STATE.s_sock)
		p->maxfd = STATE.s_sock;
	else
		p->maxfd = STATE.sock;

	FD_ZERO(&p->read_set);
	FD_SET(STATE.sock, &p->read_set);
	FD_SET(STATE.s_sock, &p->read_set);
	STATE.is_full = 0;
}


int close_socket(int sock)
{
    if (close(sock))
    {
        fprintf(stderr, "Failed closing socket.\n");
        return 1;
    }
    return 0;
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


/******************************************************************************
* subroutine: serve_error                                                     *
* purpose:    return error message to client                                  *
* parameters: client_fd: client descriptor                                    *
*             errnum: error number                                            *
*             shortmsg: short error message                                   *
*             longmsg:  long error message                                    *
*             is_closed - an indicate if sending 'Connection: close' back     *
* return:     none                                                            *
******************************************************************************/
void serve_error(int client_fd, char *errnum, char *shortmsg, char *longmsg, 
                 int is_closed) {
    struct tm tm;
    time_t now;
    char buf[MAX_LINE], body[MAX_LINE], dbuf[MIN_LINE];

    now = time(0);
    tm = *gmtime(&now);
    strftime(dbuf, MIN_LINE, "%a, %d %b %Y %H:%M:%S %Z", &tm);

    // build HTTP response body
    sprintf(body, "<html><title>Lisod Error</title>");
    sprintf(body, "%s<body>\r\n", body);
    sprintf(body, "%sError %s -- %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<br><p>%s</p></body></html>\r\n", body, longmsg);

    // print HTTP response
    sprintf(buf, "HTTP/1.1 %s %s\r\n", errnum, shortmsg);
    sprintf(buf, "%sDate: %s\r\n", buf, dbuf);
    sprintf(buf, "%sServer: Liso/1.0\r\n", buf);
    if (is_closed) sprintf(buf, "%sConnection: close\r\n", buf);
    sprintf(buf, "%sContent-type: text/html\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n\r\n", buf, (int)strlen(body));
    send(client_fd, buf, strlen(buf), 0);
    send(client_fd, body, strlen(body), 0);
}


/******************************************************************************
* subroutine: add_client                                                      *
* purpose:    add a new client to the pool and update pool attributes         *
* parameters: client_fd - the descriptor of new client                        *
*             p    - pointer to pool instance                                 *
* return:     0 on success, -1 on failure                                     *
******************************************************************************/
int add_client(int client_fd, pool *p)
{
    int i;
    p->nready--;

    if (STATE.is_full)
    	return -1;
 
    // only accept FD_SETSIZE - 5 clients to keep server from overloading
    for (i=0; i<(FD_SETSIZE - 5); i++)
    {
        if (p->clientfd[i] < 0)
        {
            // add client descriptor to the pool
            p->clientfd[i] = client_fd;

            // add the descriptor to the descriptor set
            FD_SET(client_fd, &p->read_set);

            // add read buf
             rio_readinitb(&p->clientrio[i], client_fd);

            // update max descriptor and pool highwater mark
            if (client_fd > p->maxfd)
                p->maxfd = client_fd;
            if (i > p->maxi)
                p->maxi = i;
            break;
        }
    }
    
    if (i == (FD_SETSIZE - 5))
    {   
        STATE.is_full = 1;
        Log ("Error: too many clients. \n");
        return -1;
    }
    return 0;
}

/******************************************************************************
* subroutine: assist_client                                                   *
* purpose:    process the ready set from the set of descriptors               *
* parameters: p - pointer to the pool instance                                *
* return:     0 on success, -1 on failure                                     *
******************************************************************************/
void assist_client(int i, pool *p)
{
    int connfd, is_closed;

    if((i <= p->maxi) && (p->nready > 0))
    {
        connfd = p->clientfd[i];

        if ((connfd > 0) && (FD_ISSET(connfd, &p->ready_set)))
        {
            p->nready--;
            is_closed = 0;
            process_request(i, p, &is_closed);
            /*if (is_closed) */
            remove_client(i, p);
        }
    }
}

/******************************************************************************
* subroutine: process_request                                                 *
* purpose:    handle a single request and return responses                    *
* parameters: id        - the index of the client in the pool                 *
*             p         - a pointer of pool struct                            *
*             is_closed - idicator if the transaction is closed               *
* return:     none                                                            *
******************************************************************************/
void process_request(int id, pool *p, int *is_closed)
{
    HTTPContext *context = (HTTPContext *)calloc(1, sizeof(HTTPContext));

    Log("Start processing request. \n");

    // parse request line (get method, uri, version)
    if (parse_requestline(id, p, context, is_closed) < 0)
    	goto Done;

    // check HTTP method (support GET, POST, HEAD now)
    if (strcasecmp(context->method, "GET")  && 
        strcasecmp(context->method, "HEAD") && 
        strcasecmp(context->method, "POST"))
    {
        *is_closed = 1;
        serve_error(p->clientfd[id], "501", "Not Implemented",
                   "The method is not valid or not implemented by the server",
                    *is_closed); 
        goto Done;
    }

    // check HTTP version
    if (strcasecmp(context->version, "HTTP/1.1"))
    {
        *is_closed = 1;
        serve_error(p->clientfd[id], "505", "HTTP Version not supported",
                    "HTTP/1.0 is not supported by Liso server", *is_closed);  
        goto Done;
    }

    // parse uri (get filename and parameters if any)
    parse_uri(context);
   
    // parse request headers 
    if (parse_requestheaders(id, p, context, is_closed) < 0) goto Done;

/*
    // for POST, parse request body
    if (!strcasecmp(context->method, "POST"))
        if (parse_requestbody(id, p, context, is_closed) < 0) goto Done;
*/
    // send response 
    if (!strcasecmp(context->method, "GET"))
        serve_get(p->clientfd[id], context, is_closed); 
    else if (!strcasecmp(context->method, "POST")) 
        serve_post(p->clientfd[id], context, is_closed);
    else if (!strcasecmp(context->method, "HEAD")) 
        serve_head(p->clientfd[id], context, is_closed);

    Done:
    free(context); 
    Log("End of processing request. \n");
}


/******************************************************************************
* subroutine: parse_requestline                                               *
* purpose:    parse the content of request line                               *
* parameters: id        - the index of the client in the pool                 *
*             p         - a pointer of the pool data structure                *
*             context   - a pointer refers to HTTP context                    *
*             is_closed - an indicator if the current transaction is closed   *
* return:     0 on success -1 on error                                        *
******************************************************************************/
int parse_requestline(int id, pool *p, HTTPContext *context, int *is_closed)
{
    char buf[MAX_LINE];

    memset(buf, 0, MAX_LINE); 

    if (rio_readlineb(&p->clientrio[id], buf, MAX_LINE) < 0)
    {
        *is_closed = 1;
        Log("Error: rio_readlineb error in process_request \n");
        serve_error(p->clientfd[id], "500", "Internal Server Error",
                    "The server encountered an unexpected condition.", *is_closed);
        return -1;
    }

    if (sscanf(buf, "%s %s %s", context->method, context->uri, context->version) < 3)
    {
    	Log("Method: %s\turi: %s\tversion: %s\n", context->method, context->uri, context->version);
        *is_closed = 1;
        Log("Info: Invalid request line: '%s' \n", buf);
        serve_error(p->clientfd[id], "400", "Bad Request",
                    "The request is not understood by the server", *is_closed);
        return -1;
    }

    Log("Request: method=%s, uri=%s, version=%s \n",
        context->method, context->uri, context->version);
    return 0;
}

/******************************************************************************
* subroutine: parse_uri                                                       *
* purpose:    to parse filename and CGI arguments from uri                    *
* parameters: context - a pointer of the HTTP context data structure          *
* return:     none                                                            *
******************************************************************************/
void parse_uri(HTTPContext *context)
{
    char *ptr;

    ///TODO check HTTP://
    // initialize filename path
    strcpy(context->filename, STATE.www_path);

    // parse uri
    if (!strstr(context->uri, "cgi-bin"))  // static content
    {
        context->is_static = 1;
        strcat(context->filename, context->uri);
        if (context->uri[strlen(context->uri)-1] == '/')
            strcat(context->filename, "index.html");
    }
    else
    {                             // dynamic content
        ptr = index(context->uri, '?');
        if (ptr)
        {
            strcpy(context->cgiargs, ptr+1);
            *ptr = '\0'; ///TODO what is this for?
        }
        else
        {        
            strcpy(context->cgiargs, "");
        }
    }
}

/******************************************************************************
* subroutine: parse_requestheaders                                            *
* purpose:    parse the content of request headers                            *
* parameters: id        - the index of the client in the pool                 *
*             p         - a pointer of the pool data structure                *
*             context   - a pointer refers to HTTP context                    *
*             is_closed - an indicator if the current transaction is closed   *
* return:     0 on success -1 on error                                        *
******************************************************************************/
int parse_requestheaders(int id, pool *p, HTTPContext *context, int *is_closed)
{
    int  ret, cnt = 0, has_contentlen = 0, port;
    char buf[MAX_LINE], header[MIN_LINE], data[MIN_LINE], pbuf[MIN_LINE];
    
    context->content_len = -1; 

    do
    {   
        if ((ret = rio_readlineb(&p->clientrio[id], buf, MAX_LINE)) < 0)
            break;

        cnt += ret;

        // if request header is larger than 8196, reject request
        if (cnt > MAX_LINE)
        {
            *is_closed = 1;
            serve_error(p->clientfd[id], "400", "Bad Request",
                       "Request header too long.", *is_closed);
            return -1;
        }
       
        // parse Host header
        if (strstr(buf, "Host:"))
        {
            if (sscanf(buf, "%s: %s:%s", header, data, pbuf) > 0)
            {
                port = (int)strtol(pbuf, (char**)NULL, 10);
                if (port == STATE.s_port)
                {
                    context->is_secure = 1;
                    Log("Secure connection \n");
                } 
            }
        }
 
        if (strstr(buf, "Connection: close")) *is_closed = 1;

        if (strstr(buf, "Content-Length")) 
        {
            has_contentlen = 1;
            if (sscanf(buf, "%s %s", header, data) > 0)
                context->content_len = (int)strtol(data, (char**)NULL, 10); 
            Log("Debug: content-length=%d \n", context->content_len);
        }  

    } while(strcmp(buf, "\r\n"));

    if ((!has_contentlen) && (!strcasecmp(context->method, "POST")))
    {
        serve_error(p->clientfd[id], "411", "Length Required",
                       "Content-Length is required.", *is_closed);
        return -1;
    }

    return 0;
}

/******************************************************************************
* subroutine: serve_get                                                       *
* purpose:    return response for GET request                                 *
* parameters: client_fd - client descriptor                                   *
*             context   - a pointer refers to HTTP context                    *
*             is_closed - an indicator if the current transaction is closed   *
* return:     none                                                            *
******************************************************************************/
void serve_get(int client_fd, HTTPContext *context, int *is_closed)
{

    serve_head(client_fd, context, is_closed);
    serve_body(client_fd, context, is_closed);

}

/******************************************************************************
* subroutine: serve_head                                                      *
* purpose:    return response header to client                                *
* parameters: client_fd - client descriptor                                   *
*             context   - a pointer refers to HTTP context                    *
*             is_closed - an indicator if the current transaction is closed   *
* return:     none                                                            *
******************************************************************************/
void serve_head(int client_fd, HTTPContext *context, int *is_closed)
{
    struct tm tm;
    struct stat sbuf;
    time_t now;
    char   buf[BUF_SIZE], filetype[MIN_LINE], tbuf[MIN_LINE], dbuf[MIN_LINE]; 

    if (validate_file(client_fd, context, is_closed) < 0) return;

    stat(context->filename, &sbuf);
    get_filetype(context->filename, filetype);

    // get time string
    tm = *gmtime(&sbuf.st_mtime);
    strftime(tbuf, MIN_LINE, "%a, %d %b %Y %H:%M:%S %Z", &tm);
    now = time(0);
    tm = *gmtime(&now);
    strftime(dbuf, MIN_LINE, "%a, %d %b %Y %H:%M:%S %Z", &tm);

    // send response headers to client
    sprintf(buf, "HTTP/1.1 200 OK\r\n");
    sprintf(buf, "%sDate: %s\r\n", buf, dbuf);
    sprintf(buf, "%sServer: Liso/1.0\r\n", buf);
    if (is_closed) sprintf(buf, "%sConnection: close\r\n", buf);
    sprintf(buf, "%sContent-Length: %ld\r\n", buf, sbuf.st_size);
    sprintf(buf, "%sContent-Type: %s\r\n", buf, filetype);
    sprintf(buf, "%sLast-Modified: %s\r\n\r\n", buf, tbuf);
    send(client_fd, buf, strlen(buf), 0);
}

/******************************************************************************
* subroutine: validate_file                                                   *
* purpose:    validate file existence and permisson                           *
* parameters: client_fd - client descriptor                                   *
*             context   - a pointer refers to HTTP context                    *
*             is_closed - an indicator if the current transaction is closed   *
* return:     0 on success -1 on error                                        *
******************************************************************************/
int validate_file(int client_fd, HTTPContext *context, int *is_closed)
{
    struct stat sbuf;

    // check file existence
    if (stat(context->filename, &sbuf) < 0)
    {
        serve_error(client_fd, "404", "Not Found",
                    "Server couldn't find this file", *is_closed);
        return -1;
    }

    // check file permission
    if ((!S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
    {
        serve_error(client_fd, "403", "Forbidden",
                    "Server couldn't read this file", *is_closed);
        return -1;
    }

    return 0;
}

/******************************************************************************
* subroutine: get_filetype                                                    *
* purpose:    find filetype by filename extension                             *
* parameters: filename: the requested filename                               *
*             filetype: a pointer to return filetype result                   *
* return:     none                                                            *
******************************************************************************/
void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".css"))
        strcpy(filetype, "text/css");
    else if (strstr(filename, ".js"))
        strcpy(filetype, "application/javascript");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    else
        strcpy(filetype, "text/plain");
}

/******************************************************************************
* subroutine: serve_body                                                      *
* purpose:    return response body to client                                  *
* parameters: client_fd - client descriptor                                   *
*             context   - a pointer refers to HTTP context                    *
*             is_closed - an indicator if the current transaction is closed   *
* return:     none                                                            *
******************************************************************************/
int serve_body(int client_fd, HTTPContext *context, int *is_closed)
{
    int fd, filesize;
    char *ptr;
    struct stat sbuf;
    
    if ((fd = open(context->filename, O_RDONLY, 0)) < 0)
    {
        Log("Error: Cann't open file \n");
        return -1; ///TODO what error code here should be?
    }

    stat(context->filename, &sbuf);

    filesize = sbuf.st_size;
    ptr = mmap(0, filesize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    send(client_fd, ptr, filesize, 0); 
    munmap(ptr, filesize);

    return 0;
}

/******************************************************************************
* subroutine: serve_post                                                      *
* purpose:    return response for POST request                                *
* parameters: client_fd - client descriptor                                   *
*             context   - a pointer refers to HTTP context                    *
*             is_closed - an indicator if the current transaction is closed   *
* return:     none                                                            *
******************************************************************************/
void serve_post(int client_fd, HTTPContext *context, int *is_closed)
{
    struct tm tm;
    struct stat sbuf;
    time_t now;
    char   buf[BUF_SIZE], dbuf[MIN_LINE]; 

    // check file existence
    if (stat(context->filename, &sbuf) == 0)
    {
        serve_get(client_fd, context, is_closed);
        return;
    }

    // get time string
    now = time(0);
    tm = *gmtime(&now);
    strftime(dbuf, MIN_LINE, "%a, %d %b %Y %H:%M:%S %Z", &tm);

    // send response headers to client
    sprintf(buf, "HTTP/1.1 204 No Content\r\n");
    sprintf(buf, "%sDate: %s\r\n", buf, dbuf);
    sprintf(buf, "%sServer: Liso/1.0\r\n", buf);
    if (is_closed) sprintf(buf, "%sConnection: close\r\n", buf);
    sprintf(buf, "%sContent-Length: 0\r\n", buf);
    sprintf(buf, "%sContent-Type: text/html\r\n", buf);
    send(client_fd, buf, strlen(buf), 0);
}
 
void tostring(char str[], int num)
{
    int i, rem, len = 0, n;
 
    n = num;
    while (n != 0)
    {
        len++;
        n /= 10;
    }
    for (i = 0; i < len; i++)
    {
        rem = num % 10;
        num = num / 10;
        str[len - (i + 1)] = rem + '0';
    }
    str[len] = '\0';
}


/******************************************************************************
 *                            wrappers from csapp                             *
 *****************************************************************************/

/* 
 * rio_read - This is a wrapper for the Unix read() function that
 *    transfers min(n, rio_cnt) bytes from an internal buffer to a user
 *    buffer, where n is the number of bytes requested by the user and
 *    rio_cnt is the number of unread bytes in the internal buffer. On
 *    entry, rio_read() refills the internal buffer via a call to
 *    read() if the internal buffer is empty.
 */
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n)
{
    int cnt;

    while (rp->rio_cnt <= 0) {  /* refill if buf is empty */
        rp->rio_cnt = read(rp->rio_fd, rp->rio_buf, sizeof(rp->rio_buf));
        if (rp->rio_cnt < 0) {
            if (errno != EINTR) /* interrupted by sig handler return */
                return -1;
        }
        else if (rp->rio_cnt == 0)  /* EOF */
            return 0;
        else
            rp->rio_bufptr = rp->rio_buf; /* reset buffer ptr */
    }

    /* Copy min(n, rp->rio_cnt) bytes from internal buf to user buf */
    cnt = n;
    if (rp->rio_cnt < n)
        cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

/*
 * rio_readinitb - Associate a descriptor with a read buffer and reset buffer
 */
void rio_readinitb(rio_t *rp, int fd)
{
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

/* 
 * rio_readlineb - robustly read a text line (buffered)
 */
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen)
{
    int n, rc;
    char c, *bufp = usrbuf;

    for (n = 1; n < maxlen; n++) {
        if ((rc = rio_read(rp, &c, 1)) == 1) {
            *bufp++ = c;
            if (c == '\n')
                break;
        } else if (rc == 0) {
            if (n == 1)
                return 0; /* EOF, no data read */
            else
                break;    /* EOF, some data was read */
        } else
            return -1;    /* error */
    }
    *bufp = 0;
    return n;
}

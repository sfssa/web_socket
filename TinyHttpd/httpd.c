#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdint.h>

#define ISspace(x) isspace((int)(x)) //定义了一个宏函数，使用了C库函数isspace(int c)用来判断给定的字符x是否是空格字符
#define SERVER_STRING "Server:jkdhttpd/0.1.0\r\n"//定义了一个常量字符串，包含服务器名称和版本号
#define STDIN 0//标准输入文件描述符
#define STDOUT 1//标准输出文件描述符
#define STDERR 2//错误输出文件描述符

void accept_request(void *);//处理从套接字上监听到的HTTP请求
void bad_request(int);//返回给客户端这是个错误的请求，状态码400
void cat(int ,FILE*);//读取服务器上某个文件写入socket套接字
/*
 * CGI是标准的Web服务器和应用程序之间进行通信的协议，定义了一种Web服务器和后端应用程序交互的接口，使得Web服务器可以向
 * 后端应用程序发送请求并获得请求，从而实现Web服务器的动态网站的交互。
 *
 * 主要作用是将动态网站的数据处理逻辑结构交给后端应用程序处理，而不是在Web服务器处理，通过CGI协议，Web服务器可以将来自
 * 客户端的HTTP请求传递给后端应用程序处理，然后将后端应用程序的响应返回给客户端。
 *
 * 但是由于该程序需要启动新的进程来处理每个请求，效率比较低下，容易引起服务器负载过高，现在采用了更高效、更安全的ASP、JSP
 * 等奇数实现。
 */
void cannot_execute(int);//处理发生在执行CGI程序时出现的错误
/*
 * perror是C库函数，void perror(const char *str);把一个描述性错误消息输出到标准错误stderr（stderr会保存错误信息）
 * 例如：
 * rename("test.txt","new_test.txt");//重命名文件
 * fp()=fopen("test.txt","r");//用原来的文件名来尝试打开文件
 * if(fp==NULL)
 *     perror("Error:");
 * 结果会输出：Error: : No such file or directory
 */
void error_die(const char *);//把错误信息写到perror并退出
void execute_cgi(int ,const char *,const char *,const char *);//运行CGI程序的处理
int get_line(int ,char *,int);//读取套接字的一行，把回车换行等情况统一为换行符结束
void headers(int,const char *);//把HTTP响应的头部写到套接字
void not_found(int);//处理找不到请求的文件时的情况，状态码404
void serve_file(int ,const char *);//调用cat函数将服务器上的某个文件写到套接字上并传递给client
int startup(u_short *);//初始化httpd服务，包括建立套接字，绑定端口，进行监听

/*
 * HTTP请求行：
 * 请求方法 URL 协议版本回车符换行符（注意空格）
 * HTTP响应行：
 * 协议版本 响应代号 代号描述回车符换行符
 *
 * 请求的method包括get,post,head,put,delete,connect,options,trace,patch,post
 * get          请求指定的页面信息，并返回实体主体
 * head         类似于get，只不过返回的响应中没有具体的内容，用于获取报头
 * post         向指定的资源提交数据进行处理请求，例如提交表单或者上传文件，数据被包含在请求体内，post可能导致新的资源的建立和已有资源的删除
 * put          从客户端向服务端传送的数据取代指定的文档的内容
 * delete       请求服务器删除指定的页面
 * connect      HTTP/1.1协议中预留给能够将连接改为管道方式的代理服务器
 * options      允许客户端查看服务器的性能
 * trace        回显服务器收到的请求，主要用于测试或诊断
 * patch        是对put方法的补充，用来对已知资源进行局部更新
 */
void unimplemented(int);//返回给浏览器表明收到的HTTP请求所用的method不被支持

void accept_request(void *arg)
{
    /*
     * intptr_t和uintptr_t类型用来存放指针地址，提供了可移植且安全的方法声明指针，并且和
     * 系统中使用的指针长度相同，对于把指针转化为整型形式很有用，intptr_t是为了跨平台，其长度
     * 总是所在平台的位数
     */
    int client=(intptr_t)arg;//将arg指针转化为整型形式，client是客户端socket文件描述符
    char buf[1024];//读取客户端请求数据的缓冲区
    size_t numchars;//缓冲区读取的字符数
    char method[255];//HTTP请求方法
    char url[255];//请求URL
    char path[512];//文件路径
    size_t i,j;
    struct stat st;//文件状态
    int cgi=0;//是否需要CGI处理的标志
    char *querry_string=NULL;//保存url中?后面的数据
    numchars=get_line(client,buf,sizeof(buf));//从客户端请求中读取第一行数据
    i=0,j=0;
    while(!ISspace(buf[i])&&(i<sizeof(method)-1))
    {
        //请求行
        //请求方法 URL 协议版本回车符换行符
        method[i]=buf[i];
        i++;
    }//得到请求方法
    j=i;
    method[i]='\0';
    //我们只设置了GET或POST,strcasecmp函数比较两个字符串的大小，=0代表相等
    if(strcasecmp(method,"GET")&&strcasecmp(method,"POST"))
    {
        unimplemented(client);
        return;
    }
    if(strcasecmp(method,"POST")==0)
        cgi=1;
    i=0;
    while(ISspace(buf[j])&&(j<numchars))
        j++;//跳过空格
    while(!ISspace(buf[j])&&(i<sizeof(url)-1)&&(j<numchars))
    {
        url[i]=buf[j];
        i++;
        j++;
    }
    url[i]='\0';

    if(strcasecmp(method,"GET")==0)
    {
        querry_string=url;
        while((*querry_string!='?')&&(*querry_string!='\0'))
            querry_string++;
        if(*querry_string=='?')
        {
            cgi=1;
            *querry_string='\0';
            querry_string++;
        }
    }

    sprintf(path,"htdocs%s",url);//将请求的HTTP和服务器上网站根目录htdocs拼接成一个文件路径
    if(path[strlen(path)-1]=='/')
        strcat(path,"index.html");//默认访问index网页
    if(stat(path,&st)==-1)//没有url中的文件
    {
        /*
         * 请求的第一行包括请求方法，请求URL，HTTP版本号等信息，这些信息需要
         * 通过从客户端的请求中读取，才能根据请求的URL来确定需要返回的内容或执行CGI脚本
         * 即使请求的文件不存在，也要读取这些信息，一边正确的响应客户端，这里就是从socket读取
         * 一行数据，这里读取的就是请求行，文件不存在忽略其他请求头，返回404 
         *
         * 这里之前已经调用过一次get_line函数，获取的是请求的第一行，第二次调用是为了
         * 读取并丢弃所有的请求头信息。因为请求头信息是可选的，如果客户端在请求中发送了请求头信息，
         * 则服务器必须读取并正确解析这些信息，以便做出正确的响应，找不到url中的文件需要返回404响应，
         * 正确的响应需要正确的解析请求头，并且为了保证下一次调用accept_request函数时缓冲区没有任何已经
         * 读取但未处理的信息需要丢弃请求头信息
         *
         *请求头：“GET /index.html HTTP1.1
         *请求体：Host:www.google.com
        *         USER-Agent:...都是以键值对存在
         * */
        while((numchars>0)&&strcmp("\n",buf))
            numchars=get_line(client,buf,sizeof(buf));
        not_found(client);

    }
    else//可以找到对应的文件
    {
        //如果请求的资源是个目录，在目录追加index.html
        if((st.st_mode & S_IFMT)==S_IFDIR)
            strcat(path,"/index.html");
        //是否是可执行文件
        if((st.st_mode & S_IXUSR)||(st.st_mode & S_IXOTH))
            cgi=1;
        /*
         * 如果是可执行文件则设置CGI位
         *不需要CGI处理，则发送文件内容
         *需要CGI处理，调用CGI程序发送输出
         */
        if(!cgi)//不是可执行文件，发送给客户端
            serve_file(client,path);
        else//是可执行文件，将文件通过CGI程序发送给客户端
            execute_cgi(client,path,method,querry_string);
    }
    /*
     *为什么可执行程序要通过CGI发送？
     * 当服务器收到一个HTTP请求，并需要使用动态内容时，他会尝试执行对应的可执行文件，并将输出内容返回给客户端
     * 因为可执行文件是需要执行的，而不是直接发送，通过CGI服务端可以运行可执行文件，并将结果输出返回给客户端
     */
}

void bad_request(int client)
{
    char buf[1024];
    sprintf(buf,"HTTP/1.0 400 BAD REQUEST\r\n");
    send(client,buf,sizeof(buf),0);
    sprintf(buf,"Content-type:text/html\r\n");
    send(client,buf,sizeof(buf),0);
    sprintf(buf,"\r\n");
    send(client,buf,sizeof(buf),0);
    sprintf(buf,"<p>Your browser sent a bad request");
    send(client,buf,sizeof(buf),0);
    sprintf(buf,"such as a POST without a Content-Length.\r\n");
    send(client,buf,sizeof(buf),0);
}

void cat(int client,FILE *resource)
{
    char buf[1024];
    fgets(buf,sizeof(buf),resource);
    while(!feof(resource))
    {
        send(client,buf,strlen(buf),0);
        fgets(buf,sizeof(buf),resource);
    }
}

void cannot_execute(int client)
{
    char buf[1024];

    sprintf(buf,"HTTP/1.0 500 Internal Server Error\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"Content-type:text/html\r\n");
    send(client,buf,strlen(buf),0);
    sprintf(buf,"\r\n");
    send(client,buf,strlen(buf),0); 
    sprintf(buf,"<P>Error prohibited CGI execution.\r\n");
    send(client,buf,strlen(buf),0); 
}

void error_die(const char *sc)
{
    perror(sc);
    exit(1);
}

void execute_cgi(int client,const char *path,const char *method,const char *querry_string)
{
    char buf[1024];//存储请求头信息
    int cgi_output[2];//标准输出的重定向
    int cgi_input[2];//标准输入的重定向
    pid_t pid;//进程ID
    int status;//子进程退出状态
    int i;
    char c;
    int numchars=1;
    int content_length=-1;//请求体长度
    buf[0]='A';
    buf[1]='\0';
    /*
     * 这里之所以将buf设置成一个非空的字符串，是为了正确的读取请求头的第一行
     * GET方法没有请求正文，因此读取完请求头就可以响应了
     * 这里的numchars作为循环结束的条件不能删除，删除后循环会一直进行直到出现读取错误或连接关闭等异常情况，
     * 程序会一直阻塞在读取请求头上，如果第一个字符是\0那么strcmp函数也无法比较两个字符串是否相等
    */
    if(strcasecmp(method,"GET")==0)
    {
        while((numchars>0)&& strcmp("/n",buf))
            numchars=get_line(client,buf,sizeof(buf));
    }
    //POST方法先读取请求消息的每一行，然后通过while循环读取并解析消息头的每一行，直到读取到空行，
    //主要用来读取content-length字段，可以知道请求正文的长度1
    //
    else if(strcasecmp(method,"POST")==0)
    {
        numchars=get_line(client,buf,sizeof(buf));
        while((numchars>0)&&strcmp("\n",buf))//读取并丢弃所有的头部信息
        {   //设置buf的第十六个元素为字符串结束符
            buf[15]='\0';
            if(strcasecmp(buf,"Content-Length:")==0)
               content_length=atoi(&(buf[16]));//将字符串第17个元素作为字符串开始位置，将之后的字符串转换为整型
            numchars=get_line(client,buf,sizeof(buf));
        }
        //没有正确的读取到content-length
        if(content_length==-1)
        {
            bad_request(client);
            return;
        }
    }
    //不是POST和GET，是HEAD或其他类型，则不做任何处理
    else
    {
        
    }
    //为CGI脚本输出创建一个管道，该管道将由服务器读取并发送给客户端
    if(pipe(cgi_output)<0)
    {
        cannot_execute(client);
        return;
    }
    //为CGI脚本输入创建一个管道，该管道将从客户端发送数据接收到服务器
    if(pipe(cgi_input)<0)
    {
        cannot_execute(client);
        return;
    }
    //创建子进程执行CGI脚本
    if((pid=fork())<0)
    {
        cannot_execute(client);
        return;
    }

    sprintf(buf,"HTTP/1.0 200 OK/r/n");
    send(client,buf,strlen(buf),0);
    //子进程执行CGI脚本
    if(pid==0)
    {
        //存放环境变量的数组 
        char meth_env[255];
        char query_env[255];
        char length_env[255];
        //将CGI脚本的输出重定向到管道的写端，输入重定向到管道的读端
        dup2(cgi_output[1],STDOUT);
        dup2(cgi_input[0],STDIN);
        //关闭CGI输出的读端和输入的写端
        //关闭读端或写端是为了告诉操作系统和其他进程，该进程不再需要使用该管道的该端，从而释放相关资源
        //可以避免资源泄露和不必要的资源占用，也可以帮助其他进程知道管道的状态和可用性
        //关闭读端或写端也可以告诉进程不在等待管道的该端口的输入和输出，从而防止进程在无法读取或
        //写入管道时被阻塞
        close(cgi_output[0]);
        close(cgi_input[1]);
        sprintf(meth_env,"REQUEST_METHOD=%s",method);
        putenv(meth_env);//设置请求方法并放到环境变量中
        //是GET请求，将查询字符串存储到环境变量
        if(strcasecmp(method,"GET")==0)
        {
            sprintf(query_env,"QUERY_STRING=%S",querry_string);
            putenv(query_env);
        }
        //请求是POST
        else
        {
            sprintf(length_env,"CONTENT_LENGTH=%s",content_length);
            putenv(length_env);
        }
        //执行CGI脚本
        execl(path,NULL);
        exit(0);//子进程结束
    }
    //父进程执行下面的代码
    else
    {
        //关闭CGI的输出管道写端和输入管道读端
        close(cgi_output[1]);
        close(cgi_input[0]);
        //从客户端接收请求正文，将其写入CGI脚本的输入管道
        if(strcasecmp(method,"POST")==0)
        {
            for(i=0;i<content_length;++i)
            {
                recv(client,&c,1,0);
                write(cgi_input[1],&c,1);
            }
        }
        //从CGI脚本的输出管道读取数据并发送给客户端
        while(read(cgi_output[0],&c,1)>0)
            send(client,&c,1,0);
        //关闭CGI输出管道的读端和输入管道的写端
        close(cgi_output[0]);
        close(cgi_input[1]);
        //等待子进程结束
        waitpid(pid,&status,0);
    }
}

/*
 * 在HTTP协议中结束符是\r\n，当从套接字中读取数据时可能会读到一个\r字符，此时需要进一步检查是否后面紧跟着\n，如果是
 * 那么这一行已经读取完，可以将两个字符一起丢掉，直接读下一个字符。否则就把\r换成\n保证数据完整性（完整的换行符
 * 需要\r\n）而C语言中的换行是\n因此需要进行转换
 *
 * 例如在读取数据时只读取到了回车符，没有读取到换行符，可能是因为下一个字节没有到达或者丢包等问题，导致换行符没能读取
 * 如果不对回车符进行转换，那么就会将下一个报文的第一行和当前报文的最后一行拼接到一起，导致数据解析出错
 * MSG_PEEK不移动队列中的数据而接收数据
 */
int get_line(int sock,char *buf,int size)
{
    int i=0;
    char c='\0';
    int n;
    while((i<size-1)&&(c!='\n'))
    {
        n=recv(sock,&c,1,0);
        if(n>0)
        {
            if(c=='\r')
            {
                n=recv(sock,&c,1,MSG_PEEK);
                if((n>0)&&(c=='\n'))
                    recv(sock,&c,1,0);
                else
                    c='\n';
            }
            buf[i]=c;
            i++;
        }
        else
            c='\n';
    }
    buf[i]='\0';
    return (i);
}

void headers(int client,const char* filename)
{
    /*
     *这里发现使用了strcpy和sprintf两个函数
     *int sprintf(char* str,const char* format...函数的操作对象不限于字符串，元对象可以时字符串也可以是任意基本类型的数据，主要实现将其他数据类型转换为字符串
     *char *strcpy(char *dest, const char *src)函数操作的对象是字符串，完成从源字符串到目的字符串的拷贝
     *void *memcpy(void *str1, const void *str2, size_t n)函数用来实现内存拷贝，将一块内存内容复制到零一块内存
     *memcpy常用来拷贝同种类型数据或对象
     *
     * 对于字符串来说。三个函数都可以实现，但是strcpy是最合适的，效率高且调用方便，snprintf要进行格式转化，麻烦且效率不高
     *memcpy虽然高效但是要额外提供拷贝的内存长度，易错且使用不便，长度过大还会导致性能下降
     *
     */
    char buf[1024];
    (void)filename;//让编译器不要报警告说filename变量没有使用
    strcpy(buf,"HTTP/1.0 200 OK\r\n");
    send(client,buf,strlen(buf),0);
    strcpy(buf,SERVER_STRING);
    send(client,buf,strlen(buf),0);
    sprintf(buf,"Content-Type:text/html\r\n");
    send(client,buf,strlen(buf),0);
    strcpy(buf,"\r\n");
    send(client,buf,strlen(buf),0);
}

void not_found(int client)
{
    char buf[1024];
    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "your request because the resource specified\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "is unavailable or nonexistent.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

void serve_file(int client,const char *filename)
{
    FILE *resource=NULL;//指向文件的指针
    int numchars=1;//读取的字符数
    char buf[1024];
    /*这里初始化buf是为了让while循环可以被执行，while循环结束条件是numchars>0且buf数组不等于\n，
     * 因此我们将buf数组的第一个元素赋一个任意的非换行字符，同时为第二个元素赋值为字符串的结束符\0，
     * 以保证buf中只有一个字符，通过这样的初始化，while循环在第一次迭代时读取到一个非换行字符，从而
     * 开始读取并丢弃HTTP请求报文头部
     */
    buf[0]='A';
    buf[1]='\0';
    //忽略头部文件
    while((numchars>0)&&strcmp("\n",buf))
        numchars=get_line(client,buf,sizeof(buf));
    //打开目标文件
    resource=fopen(filename,"r");
    if(resource==NULL)
        not_found(client);
    else
    {
        //传输头部信息和文件内容
        headers(client,filename);
        cat(client,resource);
    }
    fclose(resource);
}

int startup(u_short *port)
{
    int httpd=0;//服务端的socket文件描述符，会作为返回值返回到main函数
    int on=1;
    struct sockaddr_in name;//保存服务端的地址和端口号
    httpd=socket(PF_INET,SOCK_STREAM,0);
    if(httpd==-1)
        error_die("socket:");
    memset(&name,0,sizeof(name));
    name.sin_family=AF_INET;
    name.sin_port=htons(*port);
    name.sin_addr.s_addr=htonl(INADDR_ANY);
    //设置socket选项，SOL_SOCKET允许使用通用套接字，SO_REUSEADDR允许在同一端口快速重启服务器而不用等待之前的连接
    //关闭，SO_REUSEADDR用于绑定套接字前设置，可确保端口处于time_wait的时候可以重新绑定套接字，也允许多个套接字
    //绑定到同一个端口以实现IO复用
    if((setsockopt(httpd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on)))<0)
        error_die("setsocketopt:");
    if(bind(httpd,(struct sockaddr*)&name,sizeof(name))<0)
        error_die("bind:");
    if(*port==0)//如果端口号为0就动态的分配一个端口号
    {
        socklen_t namelen=sizeof(name);
        /*
         * getsockname函数用来获取一个已绑定套接字的地址
         * getsockname(int sockfd,struct sockaddr *addr,socklen_t *addrlen)
         * sockfd是已经绑定的套接字文件描述符，addr是存储地址信息的结构体指针，addrlen结构体大小指针
         * 调用成功后地址信息存储在addr所指向的结构体中，调用失败返回-1
         *
         * getsockname返回已经绑定到指定socket上的本地协议地址，通过该函数获取系统
         * 为该套接字分配的端口号，然后将该端口号赋值给*port
         *
         * 在网络编程中，将端口号设置0表示由操作系统自动分配一个未使用的端口号，例如当服务器需要监听多个
         * 端口时，但无法判断哪个端口可以使用，就可以将端口号设置为0，让操作系统自动分配
         * 在服务器程序启动时向外界公布自己的端口号为0是不可取的，无法将自动分配的端口号告知外界，因此要
         * 先调用getsockname函数获取分配的端口号后才可以告知外界
         */
        if(getsockname(httpd,(struct sockaddr*)&name,&namelen)==-1)
            error_die("getsockname:");
        *port=ntohs(name.sin_port);
    }
    if(listen(httpd,5)<0)
        error_die("listen:");
    return (httpd);//将创建好的套接字返回到main函数
}

void unimplemented(int client)
{
    char buf[1024];
    sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</TITLE></HEAD>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

/*
 * 初始化服务器端口号并创建服务器套接字
 * 进入主循环，等待客户端到来
 * 有客户端连接请求到达时，创建一个新的线程处理该请求，并返回主循环继续等待下一个连接请求
 * 主线程继续坚挺连接请求，直到收到推出信号为止
 * 关闭服务器套接字并推出
 */
int main(void)
{
    int server_sock=-1;//服务端的socket
    u_short port=9007;//服务端坚挺的端口号
    int client_sock=-1;//客户端的socket
    struct sockaddr_in client_name;//客户端的地址和端口号
    socklen_t client_name_len=sizeof(client_name);//客户端地址的长度
    pthread_t newthread;//存储新的线程的描述符
    server_sock=startup(&port);//初始化httpd服务器，建立套接字，绑定端口并进行监听
    printf("httpd running on port: %d\n",port);
    while(1)
    {
        client_sock=accept(server_sock,(struct sockaddr*)&client_name,&client_name_len);//调用accept接收客户端
        if(client_sock==-1)
            error_die("accept:");//与客户端连接失败
        //accept_request(&client_sock);//调用accept_request处理client请求
        if(pthread_create(&newthread,NULL,(void*)accept_request,(void *)(intptr_t)client_sock)!=0)//以新建线程的方式进行处理
            perror("pthread_create:");
    }
    close(server_sock);
    /*
     * 这里并不用显式的关闭client_sock，因为在accept_requesthanshu中在函数结束时会关闭客户端的套接字，
     * 这里通过新建线程进行处理，在通信结束后自动退出，释放相关资源，包括客户端的套接字
     *
     * 在这里加上client_socket关闭也可以，但是必须保证在客户端关闭套接字后才能关闭
     * 多数情况下，多次关闭一个套接字不会出错，关闭套接字本质是将该套接字从内核数据结构删除，
     * 因此重复关闭同一个套接字不会影响已经从内核数据结构中删除的套接字。但是要避免重复关闭一个
     * 未关闭的套接字（通常由代码逻辑错误导致），因此在代码中最好避免重复关闭一个套接字。
     */
    //close(client_sock);
}

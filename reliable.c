
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"

#define ACK_LEN 8
#define DATA_MIN_LEN 12
#define DATA_MAX_LEN 500

struct Sender {
    // int last_frame_sent;
    // int buffer_position;
    packet_t packet;
};

struct Reciever {
    // int last_frame_received;
    int ackno;
    // int buffer_position;
//    int max_ack;
    packet_t packet;
};

typedef struct Buffer_node {
    packet_t *pkt;
    int ack;
    int read;
    struct Buffer_node *next;
}qnode, *qlink;

typedef struct Buffer {
    qlink front, rear;
    int num;
}queue, *lqueue;

void Lenqueue (lqueue q) {
    if (q->rear == NULL) {
        q->rear = (qlink)malloc(sizeof(qnode));
        q->front = q->rear;
    } else {
        q->rear->next = (qlink)malloc(sizeof(qnode));
        q->rear = q->rear->next;
    }
    q->num++;
    q->rear->next = NULL;
}

struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */
    /* Add your own data fields below this */
    // this protocol numbers packages. once a packet is transmitted, it cannot be merged with another packet for retransmission.
    struct Sender sender;
    struct Reciever receiver;
    lqueue sender_buffer;
    lqueue receive_buffer;
    struct config_common cc;
};
rel_t *rel_list;

//    struct packet {
//        uint16_t cksum;
//        uint16_t len;
//        uint32_t ackno;
//        uint32_t seqno;        /* Only valid if length > 8 */
//        char data[500];
//    };

void initial(rel_t *r, const struct config_common *cc) {
    r->sender.packet.cksum = 0;
    r->sender.packet.len = 0;
    r->sender.packet.ackno = 1;
    r->sender.packet.seqno = 0;
    
    r->receiver.packet.cksum = 0;
    r->receiver.packet.len = 0;
    r->receiver.packet.ackno = 1;
    
    r->sender_buffer = (lqueue)malloc(sizeof(queue));
    r->sender_buffer->front = NULL;
    r->sender_buffer->rear = NULL;
    r->sender_buffer->num = 0;
    r->receive_buffer = (lqueue)malloc(sizeof(queue));
    r->receive_buffer->front = NULL;
    r->receive_buffer->rear = NULL;
    r->receive_buffer->num = 0;
    
    r->cc.single_connection = cc->single_connection;
    r->cc.timeout = cc->timeout;
    r->cc.timer = cc->timer;
    r->cc.window = cc->window;
}

/* Creates a new reliable protocol session, returns NULL on failure.
 * Exactly one of c and ss should be NULL.  (ss is NULL when called
 * from rlib.c, while c is NULL when this function is called from
 * rel_demux.) */
rel_t * rel_create (conn_t *c, const struct sockaddr_storage *ss, const struct config_common *cc) {
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
        rel_list->prev = &r->next;
    rel_list = r;

    /* Do any other initialization you need here */
    printf("creating");
    initial(r, cc);

    return r;
}

void rel_destroy (rel_t *r) {
    if (r->next)
        r->next->prev = r->prev;
    *r->prev = r->next;
    conn_destroy (r->c);
    printf("destroying");
    /* Free any other allocated memory here */
    
    //You have read an EOF from the other side (i.e., a Data packet of len 12, where the payload field is 0 bytes).
    //    You have read an EOF or error from your input (conn_input returned -1).
    //    All packets you have sent have been acknowledged.
    //    You have written all output data with conn_output.
}


/* This function only gets called when the process is running as a
 * server and must handle connections from multiple clients.  You have
 * to look up the rel_t structure based on the address in the
 * sockaddr_storage passed in.  If this is a new connection (sequence
 * number 1), you will need to allocate a new conn_t using rel_create
 * ().  (Pass rel_create NULL for the conn_t, so it will know to
 * allocate a new connection.)
 */
void rel_demux (const struct config_common *cc,
                const struct sockaddr_storage *ss,
                packet_t *pkt, size_t len) {
}

void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n) {
    //Note: You must examine the length field, and should not assume that the UDP packet you receive is the correct length. The network might truncate or pad packets.
    printf("receiving.");
    // checking length
    if (n == ACK_LEN) {
        // recieving ack package
        r->sender_buffer->front->ack = 1;
        qlink q = r->sender_buffer->front;
        r->sender_buffer->front = r->sender_buffer->front->next;
        free(q);
        r->sender_buffer->num--;
    } if (n >= DATA_MIN_LEN && n <= DATA_MAX_LEN) {
        // recieving data package
        if (r->receiver.ackno == pkt->seqno && cksum(pkt->data, pkt->len) == pkt->cksum) {
            // packet recieved & checksum succeeded
            r->receiver.packet = *pkt;
            if (r->receive_buffer->num < r->cc.window) {
                // buffer not full, ack
                Lenqueue (r->receive_buffer);
                r->receive_buffer->rear->pkt = &r->receiver.packet;
                r->receive_buffer->rear->read = 0;
                
            }
            rel_output(r);
        } // else droping
    } // else drop unqualified length package
}

void rel_read (rel_t *s) {
    printf("reading.");
    int len = conn_input(s->c, s->sender.packet.data, DATA_MAX_LEN);
    // When you read an EOF, you should send a zero-length payload (12-byte packet) to the other side to indicate the end of file condition.
    if (len == -1) {
        // You have read an EOF or error from your input
        rel_destroy(s);
    }
    s->sender.packet.len = htons(len);
    s->sender.packet.cksum = cksum(s->sender.packet.data, len);
    s->sender.packet.seqno = htons(s->receiver.ackno);
    s->sender.packet.ackno = s->sender.packet.seqno;
    
    conn_sendpkt(s->c, &s->sender.packet, sizeof(s->sender.packet));
    Lenqueue (s->sender_buffer);
    s->sender_buffer->rear->pkt = &s->sender.packet;
    s->sender_buffer->rear->ack = 0;
    
//    The length, seqno, and ackno fields are always in big-endian order (meaning you will have to use htonl/htons to write those fields and ntohl/ntohs to read them).
}

/*   * To output data you have received in decoded UDP packets, call
conn_output.  The function conn_bufspace tells you how much space
is available.  If you try to write more than this, conn_output
may return that it has accepted fewer bytes than you have asked
for.  You should flow control the sender by not acknowledging
packets if there is no buffer space available for conn_output.
The library calls rel_output when output has drained, at which
point you can send out more Acks to get more data from the remote
side.
 */
void rel_output (rel_t *r) {
    printf("outputing");
//    if success receive->ackno++;
//    When you receive a zero-length payload (and have written the contents of all previous packets), you should send an EOF to your output by calling conn_output with a len of 0.
    if (!r->receiver.packet.len) {
        conn_output(r->c, NULL, 0);
    } else {
//        To output data you have received in decoded UDP packets, call conn_output. The function conn_bufspace tells you how much space is available. If you try to write more than this, conn_output may return that it has accepted fewer bytes than you have asked for. You should flow control the sender by not acknowledging packets if there is no buffer space available for conn_output. The library calls rel_output when output has drained, at which point you can send out more Acks to get more data from the remote side.
        u_long left_size = conn_bufspace (r->c);
        if (left_size < r->receiver.packet.len) {
            // flow control
        } else {
            // ack
            int n = conn_output(r->c, r->receiver.packet.data, r->receiver.packet.len);
            assert (n > r->receiver.packet.len);
            
        }
        
        
    }

    

}

void rel_timer () {
    printf("rel_timer");
  /* Retransmit any packets that need to be retransmitted */

}

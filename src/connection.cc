/* 
 * File:   connection.cc
 * Author: leif
 *
 * Created on December 7, 2011, 4:15 PM
 */

#include "tlsperf.h"
#include "connection.h"

using namespace std;

namespace tlsperf {
    
    map<unsigned long, Connection *> _connection_map;
    
    void Connection::callback(ev::io &w, int revents)
    {
        if (EV_ERROR & revents) {
            perror("got invalid event");
            return;
        }
 
        if (revents & EV_READ)
            read_cb(w);
 
        if (revents & EV_WRITE)
            write_cb(w);
 
        m_io.set(ev::READ);
//        if (write_queue.empty()) {
//            io.set(ev::READ);
//        } else {
//            io.set(ev::READ|ev::WRITE);
//        }       
    }
    
    void Connection::write_cb(ev::io &w)
    {
        
    }
    
    void Connection::read_cb(ev::io &w)
    {
        char buffer[4096] = {0};
        int ssl_error = 0;
        size_t bytes = SSL_read(m_ssl, buffer, sizeof(buffer));
        if(bytes < 0) {
            ssl_error = SSL_get_error(m_ssl, bytes);
            switch(ssl_error) {
                case SSL_ERROR_WANT_READ:
                    return; //Ignore ssl doing it's thing
                case SSL_ERROR_SSL:
                    {
                        char errbuf[128] = {0};  // "errbuf must be at least 120 bytes long" -- ERR_error_string(3SSL)
                        ERR_error_string(ssl_error, errbuf);
                        stringstream ss;
                        ss << "{conn} " << m_id << " fd:" << m_sock_fd << " SSL_read  errors: " << errbuf;
                        while((ssl_error = ERR_get_error()) != 0) {
                            ERR_error_string(ssl_error, errbuf);
                            ss << ", ";
                            ss << errbuf;
                        }
                        ss << endl;
                        ERR(ss.str().c_str());
                        break;
                    }
                default:
                    ERR("{conn} %lu fd:%d other ssl error on SSL_read: %d\n",m_id, m_sock_fd, ssl_error);
                    break;
            }
            ERR("{conn} %lu fd:%d dropping connection due to ssl read error.\n", m_id, m_sock_fd);
            delete this; //Same as handshake must be a better way?
            return;
        }
        
        if(bytes == 0) {
            LOG("{conn} %lu fd:%d disconnected\n", m_id, m_sock_fd);
            delete this; //?? still feels icky
            return;
        }
        
        LOG("{conn} %lu fd:%d Read data:'%s' from client.\n", m_id, m_sock_fd, buffer);
    }
    
    void Connection::handshake_completed()
    {
        //OK ssl handshake completed, stop handshake io and switch to read/write cb's
        m_sslhandshake_io.stop();
        m_waiting_handshake = false;
        
        /* Disable renegotiation (CVE-2009-3555) */
        if(m_ssl->s3) {
            m_ssl->s3->flags |= SSL3_FLAGS_NO_RENEGOTIATE_CIPHERS;
        }
        
        m_handshaked = true;
        
        m_io.set<Connection, &Connection::callback>(this);
        m_io.start(m_sock_fd, ev::READ | ev::WRITE);        
    }
    
    void Connection::sslhandshake_cb(ev::io &w, int revents)
    {
        int errcode;
        int error = SSL_do_handshake(m_ssl);
        if(error == 1) 
        {
            handshake_completed();
        }
        else
        {
            errcode = SSL_get_error(m_ssl, error);
            if(errcode == SSL_ERROR_WANT_READ || errcode == SSL_ERROR_WANT_WRITE) {
                return; //Nothing to do yet come back when SSL has enough data..etc.
            }
            else if (errcode == SSL_ERROR_ZERO_RETURN) 
            {
                LOG("{conn} %lu fd:%d Connection closed in ssl handshake\n", m_id, m_sock_fd);
            }
            else
            {
                LOG("{conn} %lu fd:%d Connection closed/invalid deleting connection.\n", m_id, m_sock_fd);
                delete this; //Uh.... appears ev++ has an odditty... ? There must be a better way and still be c++
            }
        }
        
    }
    
    Connection::Connection(int s, sockaddr* addr, ev::dynamic_loop &loop, SSL_CTX * ctx):
        m_io(loop), m_sslhandshake_io(loop), 
        m_ssl(SSL_new(ctx)), m_sock_fd(s), 
        m_waiting_handshake(false),
        m_handshaked(false),
        m_renegotiation(false)
    {
        m_id = _connection_counter++;
        _connection_cnt++;
        
        LOG("{conn} %lu fd:%d got connection\n", m_id, m_sock_fd);
        
        _connection_map[m_id] = this; //Track connection object
        
        long mode = SSL_MODE_ENABLE_PARTIAL_WRITE;
#ifdef SSL_MODE_RELEASE_BUFFERS
        mode |= SSL_MODE_RELEASE_BUFFERS;
#endif
        SSL_set_mode(m_ssl, mode);
        SSL_set_accept_state(m_ssl);
        SSL_set_fd(m_ssl, m_sock_fd);
        SSL_set_app_data(m_ssl, this); //Link this connection object to the SSL state
        
        LOG("{conn} %lu fd:%d ssl setup now waiting for handshake.\n", m_id, m_sock_fd);
        
        m_sslhandshake_io.set<Connection, &Connection::sslhandshake_cb>(this);
        m_sslhandshake_io.start(m_sock_fd, ev::READ | ev::WRITE); //Write nessary? 
        m_waiting_handshake = true; //Now marked as waiting for handshake_cb to complete before callback works...
    }
    
    Connection::~Connection(void) 
    {
        Close();
    }
    
    /////
    //Node/v8 API interface
    /////
    void Connection::Initialize(Handle<Object> target)
    {
        HandleScope scope;
        Local<FunctionTemplate> t = FunctionTemplate::New(New);
        t->InstanceTemplate()->SetInternalFieldCount(1);

        NODE_SET_PROTOTYPE_METHOD(t, "close", Close);

        target->Set(String::NewSymbol("Connection"), t->GetFunction());
        
    }
    
    Handle<Value> Connection::New(const Arguments& args)
    {
        HandleScope scope;
        assert(args.IsConstructCall());
        return v8::ThrowException(v8::Exception::Error(
                String::New("cannot create a connection object directly!")));
        
        return scope.Close(args.This());
    }
    
    void Connection::Close()
    {
        if(m_waiting_handshake) 
        {
            m_sslhandshake_io.stop();
        }
        else
        {
            m_io.stop();
        }
        
        SSL_set_shutdown(m_ssl, SSL_SENT_SHUTDOWN);
        SSL_free(m_ssl);
        
        close(m_sock_fd);
        
        LOG("{conn} %lu fd:%d disconnected\n", m_id, m_sock_fd);
        
        _connection_map.erase(m_id); //Remove our object from tracking map where deleted duh!
        _connection_cnt--;        
    }
    
    Handle<Value> Connection::Close(const Arguments& args)
    {
        HandleScope scope;
        node::ObjectWrap::Unwrap<Connection>(args.This())->Close();
        return Undefined();
    }
    
} /* namespace tlsperf */
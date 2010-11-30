#include "p2engine/rdp/urdp_flow.hpp"
#include "p2engine/rdp/const_define.hpp"
#include "p2engine/io.hpp"
#include "p2engine/safe_buffer_io.hpp"

#include <iostream>

#if defined(_MSC_VER)&&_MSC_VER<=1500
# pragma warning (push)
# pragma warning(disable : 4267)//min max warning
#endif

NAMESPACE_BEGIN(p2engine)
NAMESPACE_BEGIN(urdp)

namespace
{
	const uint32_t MIN_RTO   =   250; // 250 ms(RFC1122,Sec 4.2.3.1 "fractions of a second")
	const uint32_t DEF_RTO   =   3*1000; // 3 seconds (RFC1122, Sec 4.2.3.1)
	const uint32_t MAX_RTO   =   60*1000;// 60 seconds
	const uint32_t ACK_DELAY =   100; // 100 milliseconds

	const uint32_t DEFAULT_TIMEOUT = 10*1000; 
	const uint32_t CLOSED_TIMEOUT = 60 * 1000; // If the connection is closed, once per minute

	const uint32_t IDLE_PING = 600 * 1000; //(note: WinXP SP2 firewall udp timeout is 90 seconds)
	const uint32_t IDLE_TIMEOUT = 1200 * 1000; //;

	inline uint32_t bound(uint32_t lower, uint32_t middle, uint32_t upper) 
	{	
		return std::min(std::max(lower,middle),upper);
	}
	inline uint32_t NOW()
	{
		uint32_t ret=(uint32_t) ((0xffffffffULL)&rough_local_tick_count());
		if (ret==0)//0��flow������������Ϊһ����Ч��ʶ����������0��
			return 1;
		return ret;
	}
}

boost::shared_ptr<urdp_flow> 
urdp_flow::create_for_active_connect(connection_sptr sock, io_service& ios,
									 const endpoint_type& local_edp, 
									 error_code& ec )
{
	shared_ptr obj(new this_type(ios), shared_access_destroy<this_type>());
	obj->m_is_active=true;
	obj->m_socket=sock.get();
	obj->m_state=TCP_INIT;
	obj->m_self_holder=obj;//hold self
	obj->m_token=shared_layer_type::create_flow_token(ios,local_edp,obj.get(),
		boost::bind(&this_type::called_by_sharedlayer_on_recvd,obj.get(),_1,_2),
		ec);
	return obj;
}

boost::shared_ptr<urdp_flow> 
urdp_flow::create_for_passive_connect(io_service& ios, acceptor_sptr acceptor, 
									  shared_layer_sptr sharedLayer, 
									  const endpoint_type& remote_edp, 
									  error_code& ec )
{
	shared_ptr obj(new this_type(ios), shared_access_destroy<this_type>());
	obj->m_acceptor=acceptor;
	obj->m_domain=acceptor->get_domain();
	obj->m_remote_endpoint=remote_edp;
	obj->m_is_active=false;
	obj->m_state=TCP_LISTEN;
	obj->m_self_holder=obj;//hold self
	obj->m_token=shared_layer_type::create_flow_token(sharedLayer,obj.get(),
		boost::bind(&this_type::called_by_sharedlayer_on_recvd,obj.get(),_1,_2),
		ec);
	obj->m_timer->async_wait(seconds(20));//if nothing happened in 20s, close
	return obj;
}

urdp_flow::urdp_flow(io_service& ios)
: basic_engine_object(ios)
, m_state(TCP_INIT)
, in_speed_meter_(milliseconds(2000))
, out_speed_meter_(milliseconds(2000))
, remote_to_local_lost_rate_(-1)
, local_to_remote_lost_rate_(-1)
{
	set_obj_desc("urdp_flow");
	__init();
}

void urdp_flow::called_by_sharedlayer_on_recvd(safe_buffer& buf, 
											   const endpoint_type& from)
{
	__process(buf,from);
	if(m_shutdown==SD_GRACEFUL)
	{
		if (m_socket&&m_retrans_slist.empty()&&m_slist.empty())
		{
			m_socket=NULL;
			m_state=TCP_CLOSING;
			m_close_base_time=NOW()+DEFAULT_TIMEOUT;
			__packet_reliable_and_sendout(m_snd_nxt,CTRL_FIN,NULL,0);
			__schedul_timer();
		}	
	}
}

void urdp_flow::__init()
{
	uint32_t now = NOW();

	next_op_stamp();
	
	b_keep_recving_=false;

	if (m_timer)
	{
		m_timer->cancel();
		m_timer->time_signal().clear();
	}
	m_timer=timer_type::create(get_io_service());
	m_timer->time_signal().bind(&this_type::__on_clock,this);
	m_close_base_time=now+20*1000;//we will close this if not connected in 20 seconds.

	m_shutdown=SD_NONE;
	m_error=0;
	m_dissconnect_reason=__DISCONN_INIT;
	m_socket=NULL;
	//m_acceptor=NULL;

	m_remote_peer_id=get_invalid_peer_id_vistor<packet_format_type>()();
	m_ping_interval=DEFAULT_TIMEOUT;//5s

	m_state = TCP_INIT;
	m_snd_wnd=RECV_BUF_SIZE/2;
	m_rcv_wnd = RECV_BUF_SIZE;
	m_snd_una=m_snd_nxt =random<uint32_t>(0xff,0x7fffffff);
	m_slen = 0;
	//m_rcv_nxt=0;//it will be inited when shakehand
	m_rlen = 0;
	m_detect_readable = true;
	m_detect_writable = false;
	m_t_ack = 0;

	//m_msslevel = 0;
	//m_largest = 0;
	//m_mtu_advise = MAX_PACKET;
	m_mss =MTU_SIZE;//DEFAULT_MSS;

	m_rto_base = 0;

	m_cwnd = 2 * m_mss;
	m_ssthresh = RECV_BUF_SIZE;
	m_lastrecv = m_lastsend = m_lasttraffic = now;

	m_dup_acks = 0;
	m_recover = 0;

	m_ts_recent =0;
	m_ts_lastack =m_rcv_nxt;

	m_rx_rto = DEF_RTO;
	m_rx_srtt = m_rx_rttvar = 0;//0��ʾδ֪��С

	m_session_id=random<uint16_t>(0,0xffff);

	m_timer_posted=false;
	//BOOST_ASSERT(m_udplayer);
}

urdp_flow::~urdp_flow() 
{
	BOOST_ASSERT(m_state==TCP_CLOSED);
}

bool 
urdp_flow::is_open()const
{
	if (m_token)
		return m_token->shared_layer->is_open();
	return false;
}

bool 
urdp_flow::is_connected()const
{
	return is_open()&&m_state==TCP_ESTABLISHED;
}

int 
urdp_flow::flow_id()const
{
	if (is_open())
		return m_token->flow_id;
	return get_invalid_peer_id_vistor<packet_format_type>()();
}

void  
urdp_flow::async_connect(const endpoint_type& remoteEnp,
						 const std::string& domainName,
						 error_code& ec,
						 const time_duration& time_out
						 )
{
	BOOST_ASSERT(m_token);
	ec.clear();
	if (m_state >= TCP_LISTEN) 
	{
		if (m_state==TCP_SYN_SENT)
		{
			m_error = asio::error::in_progress;
			ec=asio::error::in_progress;
		}
		else if (m_state==TCP_ESTABLISHED)
		{
			m_error = asio::error::already_connected;
			ec=asio::error::already_connected;
		}
		else
		{
			m_error = asio::error::invalid_argument;
			ec=asio::error::invalid_argument;
		}
	}
	if (ec)
	{
		boost::function<void()> handler(
			boost::bind(&this_type::__allert_connected,this,ec)
			);
		get_io_service().post(boost::bind(&this_type::dispatch_handler,
			SHARED_OBJ_FROM_THIS,handler,op_stamp()));
	}

	if (time_out==boost::date_time::pos_infin)
	{
		m_timer->async_wait(seconds(20));//if nothing happened in 20s, close
		m_close_base_time=NOW()+20*1000;
	}
	else
	{
		m_timer->async_wait(time_out);
		m_close_base_time=NOW()+total_milliseconds(time_out);
	}
	m_remote_endpoint=remoteEnp;
	m_domain=domainName;
	m_is_active=true;
	m_state = TCP_SYN_SENT;
	
	//send connect request 
	__queue(domainName.c_str(),domainName.length(),CTRL_CONNECT);
	__attempt_send();
	__schedul_timer();
}

void urdp_flow::keep_async_receiving()
{
	if (false==b_keep_recving_)
	{
		b_keep_recving_=true;
		get_io_service().post(
			boost::bind(&this_type::__do_async_receive,SHARED_OBJ_FROM_THIS,op_stamp())
			);
	}
}

void urdp_flow::block_async_receiving()
{
	b_keep_recving_=false;
}

void urdp_flow::__do_async_receive(boost::int64_t mark)
{
	if (is_canceled_op(mark))
		return;
	if (!b_keep_recving_)
		return;

	error_code ec;
	safe_buffer buf;
	__recv(buf,ec);
	__schedul_timer();
	if (!ec)
	{
		__allert_received(buf);
		__do_async_receive(mark);
	}
	else
	{
		if (ec != asio::error::would_block
			&& ec != asio::error::try_again)
		{
			__allert_disconnected();
		}
	}
}

void urdp_flow::async_send(const safe_buffer& buf, message_type msgType
						   ,boost::logic::tribool reliable)
{
	error_code ec;
	__send(buf,msgType,reliable,ec);
	if (ec)
	{
		if (ec == asio::error::would_block || ec == asio::error::try_again)
		{
			//do nothing? 
		}
		else
		{
			boost::function<void()> handler(
				boost::bind(&this_type::__allert_disconnected,this)
				);
			get_io_service().post(boost::bind(&this_type::dispatch_handler,
				SHARED_OBJ_FROM_THIS,handler,op_stamp()));
		}
	}
	else if(reliable)
	{
		boost::function<void()> handler(
			boost::bind(&this_type::__allert_writeable,this)
			);
		get_io_service().post(boost::bind(&this_type::dispatch_handler,
			SHARED_OBJ_FROM_THIS,handler,op_stamp()));
	}
	__schedul_timer();
}


safe_buffer urdp_flow::make_punch_packet(error_code& ec,const endpoint& externalEdp)
{
	if (is_open())
	{
		uint32_t now = NOW();

		safe_buffer buffer(reliable_packet_format_type::packet_size());
		packet_writer<reliable_packet_format_type> writer(buffer);
		reliable_packet_format_type& h=writer.packet_format_def();
		h.set_control(CTRL_PUNCH);
		h.set_peer_id(m_token->flow_id);
		//TODO:
		//h.set_bandwidth_recving();
		h.set_lostrate_recving(remote_to_local_lost_rate_/LOST_RATE_PRECISION);
		h.set_id_for_lost_detect(id_for_lost_rate_++);
		h.set_session_id(m_session_id);
		h.set_window((uint16_t)std::min(m_rcv_wnd,(uint32_t)0xffff));//any value
		h.set_time_sending((uint16_t)(now?now:1));//any value
		h.set_time_echo(m_ts_recent+((uint16_t)(now)-m_ts_recent_now));//any value
		h.set_seqno(m_snd_nxt);//any value
		h.set_ackno(m_rcv_nxt);//any value

		//write local_endpoint
		safe_buffer_io io(&buffer);
		io<<externalEdp;
		if (ec) return safe_buffer();
		return buffer;
	}
	else
	{
		ec=boost::asio::error::bad_descriptor;
		return safe_buffer();
	}
}

void urdp_flow::on_received_punch_request(const safe_buffer& buf)
{
	if (buf.length()<reliable_packet_format_type::packet_size())
		return;
	safe_buffer endpoint_buf=buf.buffer_ref(reliable_packet_format_type::packet_size());
	endpoint_type remote_edp;
	safe_buffer_io io(&endpoint_buf);
	io.read_v4_endpoint(remote_edp);

	error_code ec;
	if(m_token&&remote_edp.port())
	{
		for (int i=0;i<3;++i)//send 3 times
			m_token->shared_layer->send_to_imeliately(
			buf.buffer_ref(0,packet_format_type::packet_size()),remote_edp,ec
			);
	}
}


void urdp_flow::__on_clock() 
{
	if (m_state == TCP_CLOSED)
		return;
	m_timer_posted=false;
	uint32_t now=NOW();
	bool haveSentMsg=false;

	//check if it's time to close
	if(m_close_base_time && *m_close_base_time<now)
	{
		__packet_reliable_and_sendout(m_snd_nxt,CTRL_RST,NULL,0);
		__allert_disconnected();
		return;
	}

	// Check for idle timeout
	if ((m_state == TCP_ESTABLISHED) 
		&& (mod_minus((m_lastrecv+m_ping_interval+bound(m_ping_interval,3000,DEFAULT_TIMEOUT)), now) <= 0)) 
	{
		__allert_disconnected();
		return;
	}

	//is it time to send next unreliable packet?
	while(!m_unreliable_slist.empty())
	{
		SUnraliableSegment& seg = m_unreliable_slist.front();
		if (seg.timeout<=now)
		{
			__packet_unreliable_and_sendout(seg);
			if (seg.remainXmit<=0)
				m_unreliable_slist.pop_front();
		}
		else
			break;
	}

	// resend reliable packets?
	if (m_rto_base && mod_minus(m_rto_base+m_rx_rto, now)<=0) 
	{
		if (m_retrans_slist.empty()) 
		{
			BOOST_ASSERT(false);//m_rto_base��Ϊ0ʱ��Ӧ��һ����δȷ�ϰ�
		} 
		else 
		{
			//�������ϵ��Ǹ�δ��ȷ�ϵ����ݰ�
			if (!__transmit(m_retrans_slist.begin(), now)) 
			{
				__allert_disconnected();
				return;
			}
			BOOST_ASSERT(mod_minus(m_snd_nxt,m_snd_una)>0);
			uint32_t nInFlight =mod_minus(m_snd_nxt,m_snd_una);
			m_ssthresh = std::max(nInFlight*3/4, 2*m_mss);
			m_cwnd=2*m_mss;//m_cwnd = std::min(m_ssthresh/2,2*m_mss)??//��׼������=mss
			m_rto_base=(now?now:1);//now���������Ϊ0����m_rto_base=0��˼Ϊû�����ã���˵�nowΪ0ʱ����һ����0�ӽ�����1
			__incress_rto();
			haveSentMsg=true;
		}
	}

	// Check if it's time to probe closed windows
	if ((m_snd_wnd == 0) 
		&& (mod_minus(m_lastsend + m_rx_rto, now) <= 0)
		) 
	{
		if (mod_minus(now, m_lastrecv) >= 15000)//����Է��Ľ��ն���15���ڶ�û����ͨ�������������� 
		{
			__allert_disconnected();
			return;
		}
		if (!haveSentMsg)
		{
			// probe the window
			__packet_reliable_and_sendout(m_snd_nxt-1,CTRL_ACK,0,0);
			haveSentMsg=true;
		}
		// back off retransmit timer
		__incress_rto();
	}

	// Check if it's time to _send delayed acks
	if (!haveSentMsg&&m_t_ack && (mod_minus(m_t_ack+ACK_DELAY, now) <= 0)) 
	{
		__packet_reliable_and_sendout(m_snd_nxt,CTRL_ACK,0,0);
		haveSentMsg=true;
	}

	// Check for ping timeout 
	uint32_t pingBaseTime=(m_is_active?m_lastsend:(m_lastrecv+3000));
	if (!haveSentMsg
		&&(m_state == TCP_ESTABLISHED) 
		&&mod_less(pingBaseTime+m_ping_interval, now)
		&&m_slist.empty()
		&&m_retrans_slist.empty()
		) 
	{
		//����PING��������reliable���У�����������ط�ʧ�ܾͻ�֪���Է�ʧЧ
		//;����ʹ��һ��CTRL_ACK�������Я��һ���ֽڣ�ʹ�öԷ�����ack.
		char dummy[1];
		__queue(dummy,1,CTRL_ACK);
		__attempt_send();
		haveSentMsg=true;
	}

	//������һ�ζ�ʱ
	__schedul_timer(true);
}

void  
urdp_flow::ping_interval(const time_duration& t)
{
	bool scheduTime=m_ping_interval>total_milliseconds(t);
	m_ping_interval=total_milliseconds(t);
	if (scheduTime)
		__schedul_timer();
}

time_duration  
urdp_flow::ping_interval()const
{
	return milliseconds(m_ping_interval);
}

void urdp_flow::ping(error_code& ec)
{
	if (!is_connected())
	{
		ec=boost::asio::error::not_connected;
		return;
	}
	if (m_slist.empty()&&m_retrans_slist.empty())
	{
		char dummy[1];
		__queue(dummy,1,CTRL_ACK);
		__attempt_send();
	}
}


urdp_flow::endpoint_type 
urdp_flow::local_endpoint(error_code& ec)const
{
	if (is_open())
		return m_token->shared_layer->local_endpoint(ec);
	ec=asio::error::not_socket;
	return endpoint_type();
}

urdp_flow::endpoint_type 
urdp_flow::remote_endpoint(error_code& ec)const
{
	if (is_open())
		return m_remote_endpoint;
	ec=asio::error::not_socket;
	return endpoint_type();
}

void urdp_flow::__close(bool graceful) 
{
	set_cancel();

	error_code ec;
	m_dissconnect_reason|=DISCONN_LOCAL;
	m_socket=NULL;

	if (!m_token||SD_NONE!=m_shutdown||m_state==TCP_CLOSED)
	{
		__to_closed_state();
		return;
	}

	if (!graceful||m_state<TCP_ESTABLISHED)
	{
		m_shutdown=SD_FORCEFUL;
		//�رգ�����Ѹ����Է���������reset
		__packet_reliable_and_sendout(m_snd_nxt,CTRL_RST,NULL, 0);
		__packet_reliable_and_sendout(m_snd_nxt,CTRL_RST,NULL, 0);
		__to_closed_state();
	}
	else
	{
		m_shutdown=SD_GRACEFUL;
		if (!m_close_base_time&&m_retrans_slist.empty()&&m_slist.empty())
		{
			m_state=TCP_CLOSING;
			m_close_base_time=NOW()+DEFAULT_TIMEOUT;
			__packet_reliable_and_sendout(m_snd_nxt,CTRL_FIN,NULL,0);
			__schedul_timer();
		}	
	}
}

error_code 
urdp_flow::get_error() 
{
	return error_code(m_error,asio::error::system_category);
}

double urdp_flow::alive_probability()const
{
	if (!is_open())
		return 0.0;
	uint32_t now=NOW();
	//this connection is just created, just believe it to be alive
	if (m_establish_time+3000>now) 
		return 1.0;
	double a=mod_minus(now,m_lastrecv)+350;
	double n=m_ping_interval+350;
	double p=a/n;
	if (p>=5.0) 
		return 0.0;
	return 1.0/(p*p+1e-6);
}


double urdp_flow::local_to_remote_lost_rate() const
{
	return local_to_remote_lost_rate_;
}

//
// Internal Implementation
//
void 
urdp_flow::__schedul_timer(bool calledInOnClock)
{
	uint32_t now=NOW();
	long t=0xffffff;
	if (__clock_check(now,t))
	{
		if(t<=0)
		{
			if(!m_timer_posted)
			{
				get_io_service().post(boost::bind(&this_type::__on_clock,SHARED_OBJ_FROM_THIS));
				m_timer_posted=true;
			}
			return;
		}
		timer_type::duration_type du=m_timer->expires_from_now();
		if (m_timer->is_idle()||
			du>seconds(0)&&du>milliseconds(t+5)
			)//t+5��˼�Ǻ���5ms�Ķ�ʱ���
			m_timer->async_wait(milliseconds(t));
	}
}

int 
urdp_flow::__can_let_transport_read() 
{
	if (m_state != TCP_ESTABLISHED) 
		return -1;//socket �رգ�ʹ��transport�ɶ���transport��ȡʱ����⵽����

	//���Ȳ鿴��û�з�reliable�İ�
	if (!m_unreliable_rlist.empty())
	{
		return (int)m_unreliable_rlist.front().size();
	}


	//���reliable��
	if (m_rlen < 4) //lent(2byte),msgType(2byte)
		return 0;
	//��ȡreliable������,��ȡpacket����
	RSegmentList::iterator itr(m_rlist.begin());
	char bufForLen[2];
	if (itr->buf.size()>=2)
	{
		bufForLen[0]=buffer_cast<char*>(itr->buf)[0];
		bufForLen[1]=buffer_cast<char*>(itr->buf)[1];
	}
	else if (itr->buf.size()==1)
	{
		bufForLen[0]=buffer_cast<char*>(itr->buf)[0];
		++itr;
		bufForLen[1]=buffer_cast<char*>(itr->buf)[0];//0,not 1
	}
	else 
	{
		BOOST_ASSERT(0);
	}
	char* pbufForLen=bufForLen;
	uint16_t packetLen=read_uint16(pbufForLen);
	uint32_t maxReadLen = packetLen+4;//�����ǲ�����ͷ�ģ���+4
	if (maxReadLen>m_rlen)
	{
		return 0;
	}
	return maxReadLen;
}

int urdp_flow::__recv(safe_buffer &buf,error_code& ec) 
{
	if (!m_socket)
	{
		BOOST_ASSERT(0);
		m_error = asio::error::not_connected;
		ec=asio::error::not_connected;
		return -1;
	}

	if (m_state != TCP_ESTABLISHED) 
	{
		m_error = asio::error::not_connected;
		ec=asio::error::not_connected;
		return -1;
	}

	//����Ƿ�����������packet
	int maxReadLen =__can_let_transport_read();
	if (maxReadLen==0)
	{
		m_detect_readable = true;
		m_error = asio::error::would_block;
		ec=asio::error::would_block;
		return -1;
	}
	BOOST_ASSERT(maxReadLen>0);

	//���ȶ�ȡunreliable�����ݰ�
	if (!m_unreliable_rlist.empty())
	{
		buf=m_unreliable_rlist.front();
		m_unreliable_rlist.pop_front();
		return (int)buf.size();
	}

	//��ȡreliable�����ݰ�
	safe_buffer_io io(&buf);
	io.prepare(maxReadLen);
	int readLen=0;
	while (maxReadLen-readLen>0)
	{
		RSegment& segment=const_cast<RSegment&>(*m_rlist.begin());
		int lenth=(std::min<int>)(maxReadLen-readLen,segment.buf.size());
		io.write(buffer_cast<char*>(segment.buf),lenth);
		segment.buf=segment.buf.buffer_ref(lenth);//consume(lenth);
		m_rlen -=lenth;
		readLen+=lenth;

		if (segment.buf.size()==0)
		{
			m_rlist.erase(m_rlist.begin());
		}
		else
		{
			RSegment cpy=segment;
			cpy.seq+=lenth;
			m_rlist.erase(m_rlist.begin());
			m_rlist.insert(cpy);
			break;
		}
	}
	BOOST_ASSERT(maxReadLen==readLen);

	if ((RECV_BUF_SIZE - m_rlen - m_rcv_wnd) >=(std::min<uint32_t>)(RECV_BUF_SIZE / 2, m_mss)) 
	{
		bool bWasClosed = (m_rcv_wnd == 0); // !?! Not sure about this was closed business

		m_rcv_wnd = RECV_BUF_SIZE - m_rlen;

		if (bWasClosed) 
			__attempt_send(sfImmediateAck);
	}

	uint16_t msgLen;
	io>>msgLen;//�������ֶ�ȥ��
	BOOST_ASSERT(msgLen+2==buf.length());

	return maxReadLen-2;
}

int urdp_flow::__send(safe_buffer buf,uint16_t msgType,
					  boost::logic::tribool reliable, error_code& ec)
{
	if (m_state != TCP_ESTABLISHED||!m_socket)
	{
		BOOST_ASSERT(m_socket);
		m_error = asio::error::not_connected;
		ec=asio::error::not_connected;
		return -1;
	}

	if (reliable)//reliable
	{
		BOOST_ASSERT(reliable.value==boost::logic::tribool::true_value);

		//д��packet chunkͷ
		char header[4];
		char* ptoHeader=header;
		write_uint16((uint16_t)buf.size(),ptoHeader);
		write_uint16((uint16_t)msgType,ptoHeader);
		__queue(header,4,CTRL_DATA,buf.size());//д��header��Ԥ��len���������
		int written = __queue(buffer_cast<char*>(buf),(uint32_t)buf.size(),CTRL_DATA,0);//д������
		__attempt_send();

		if (m_slen >= SND_BUF_SIZE) 
		{
			m_detect_writable = true;
			m_error = asio::error::would_block;
			ec=asio::error::would_block;
			return -1;
		}
		return written;
	}
	else//semireliable&unreliable
	{
		if (buf.size()>m_mss||buf.size()==0)
			return (int)buf.size();//������ȳ��ȴ���mss����ֱ�Ӷ���������

		SUnraliableSegment seg;
		seg.buf=buf;
		seg.timeout=NOW()+random(30,80);//30~80ms���ط�һ��
		seg.msgType=msgType;
		seg.pktID=++m_unreliable_pktid;

		if (!reliable)//unreliable
		{
			BOOST_ASSERT(reliable.value==boost::logic::tribool::false_value);
			seg.control=CTRL_UNRELIABLE_DATA;
			seg.remainXmit=1;
		}
		else//semireliable
		{
			BOOST_ASSERT(reliable.value==boost::logic::tribool::indeterminate_value);
			seg.control=CTRL_SEMIRELIABLE_DATA;
			seg.remainXmit=2;
		}
		__packet_unreliable_and_sendout(seg);
		if (seg.remainXmit>0)
			m_unreliable_slist.push_back(seg);
		return (int)buf.size();//������ȳ��ȴ���mss����ֱ�Ӷ���������
	}
}

uint32_t urdp_flow::__queue(const char * data, std::size_t len, 
							uint8_t ctrlType, std::size_t reserveLen) 
{
	std::size_t writeLen=0;
	while (len-writeLen>0)
	{
		// We can concatenate data if the last segment is the same type
		// (control v. regular data), and has not been transmitted yet
		if (ctrlType==CTRL_DATA//ֻ�����ݲ�����������control����Ӧ�õ�������
			&& !m_slist.empty() 
			&& (m_slist.back().xmit == 0)
			&& (m_slist.back().ctrlType == ctrlType) 
			&& (m_slist.back().buf.size()<m_mss)
			//&& (m_slist.back().buf.size()-m_mss>8)//���ÿռ�̫С����
			) 
		{
			safe_buffer& bf=m_slist.back().buf;
			std::size_t canWriteLen=std::min(m_mss-bf.size(),len-writeLen);
			safe_buffer_io io(&bf);
			io.write(data+writeLen,canWriteLen);
			writeLen+=canWriteLen;
			m_slen+=(uint32_t)canWriteLen;
		}
		else 
		{
			std::size_t canWriteLen=std::min(m_mss,len-writeLen);
			std::size_t newBufferLen=canWriteLen;
			if (writeLen+canWriteLen==len)//��Ϊ���д��ʱ��new buffer�ĳ���ҪԤ��reserveLen
			{
				newBufferLen=std::min(m_mss,canWriteLen+reserveLen);
				//��һ�����ڳ���,���Ҫnew�ĳ����Ѿ�����newBufferLen�������Ϊm_mss
				if (newBufferLen>(m_mss/2))
					newBufferLen=m_mss;
			}
			SSegment sseg(m_snd_una + m_slen,ctrlType);
			safe_buffer_io io(&sseg.buf);
			io.prepare(newBufferLen);
			io.write(data+writeLen,canWriteLen);
			writeLen+=canWriteLen;
			m_slen+=(uint32_t)canWriteLen;
			m_slist.push_back(sseg);
			//std::cout<<"~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~"<<sseg.buf.size()<<std::endl;
		}
	}
	return (uint32_t)writeLen;
}

int
urdp_flow::__packet_unreliable_and_sendout(SUnraliableSegment& seg) 
{
	uint32_t now = NOW();

	packet_writer<unreliable_packet_format_type> urdp_header;
	unreliable_packet_format_type& h=urdp_header.packet_format_def();
	h.set_control(seg.control);
	h.set_peer_id(seg.control==CTRL_CONNECT?m_token->flow_id:m_remote_peer_id);
	//TODO:
	//h.set_bandwidth_recving();
	h.set_lostrate_recving(remote_to_local_lost_rate_/LOST_RATE_PRECISION);
	h.set_id_for_lost_detect(id_for_lost_rate_++);
	h.set_session_id(m_session_id);
	h.set_window((uint16_t)std::min(m_rcv_wnd,(uint32_t)0xffff));


	//д��packet chunkͷ
	char header[4];
	char* ptoHeader=header;
	write_uint16((uint16_t)seg.pktID,ptoHeader);
	write_uint16((uint16_t)seg.msgType,ptoHeader);

	boost::array<asio::const_buffer,3> bufVec;
	bufVec[0]=asio::buffer(buffer_cast<char*>(urdp_header.buffer())
		,unreliable_packet_format_type::packet_size());
	bufVec[1]=asio::buffer(header,4);
	bufVec[2]=asio::buffer(buffer_cast<const char*>(seg.buf),seg.buf.length());
	error_code ec;//FIXME�������˴��󣬺��ʣ�����
	out_speed_meter_+=(seg.buf.length()+unreliable_packet_format_type::packet_size());
	m_token->shared_layer->send_to_imeliately(bufVec,m_remote_endpoint,ec);

	seg.remainXmit--;
	m_lasttraffic=m_lastsend = now;

	return (int)seg.buf.length();
}


int
urdp_flow::__packet_reliable_and_sendout(uint32_t seq,uint8_t control,
										 const char * data,uint32_t len
										 ) 
{
	uint32_t now = NOW();

	packet_writer<reliable_packet_format_type> urdp_header;
	reliable_packet_format_type& h=urdp_header.packet_format_def();
	h.set_control(control);
	h.set_peer_id(control==CTRL_CONNECT?m_token->flow_id:m_remote_peer_id);
	//TODO:
	//h.set_bandwidth_recving();
	h.set_lostrate_recving(remote_to_local_lost_rate_/LOST_RATE_PRECISION);
	h.set_id_for_lost_detect(id_for_lost_rate_++);
	h.set_session_id(m_session_id);
	h.set_window((uint16_t)std::min(m_rcv_wnd,(uint32_t)0xffff));
	h.set_time_sending((uint16_t)(now?now:1));
	h.set_time_echo(m_ts_recent+((uint16_t)(now)-m_ts_recent_now));
	h.set_seqno(seq);
	h.set_ackno(m_rcv_nxt);

	if (data)
	{
		boost::array<asio::const_buffer,2> bufVec;
		bufVec[0]=asio::buffer(buffer_cast<char*>(urdp_header.buffer())
			,reliable_packet_format_type::packet_size());
		bufVec[1]=asio::buffer(data,len);
		error_code ec;//FIXME�������˴��󣬺��ʣ�����
		m_token->shared_layer->send_to_imeliately(bufVec,m_remote_endpoint,ec);
	}
	else
	{
		error_code ec;//FIXME�������˴��󣬺��ʣ�����
		m_token->shared_layer->send_to_imeliately(urdp_header.buffer(),
			m_remote_endpoint,ec);
	}
	m_ts_lastack = m_rcv_nxt;
	m_t_ack = 0;//���͸��Է�һ�����ݰ�������к���ack����������m_t_ackΪ0����˼�����յ��µİ�ǰ������Ҫ����Է�����ack
	m_lasttraffic=m_lastsend = now;
	out_speed_meter_+=(len+reliable_packet_format_type::packet_size());
	return len;
}

bool
urdp_flow::__clock_check(uint32_t now, long& nTimeout) 
{
	if (m_shutdown == SD_FORCEFUL)
		return false;


	//if ((m_shutdown == SD_GRACEFUL && m_state != TCP_ESTABLISHED)
	//	&&(m_slen == 0 && m_t_ack == 0)
	//	)
	//{
	//	return false;
	//}

	if (m_state == TCP_CLOSED)
		return false;

	nTimeout =std::max(m_ping_interval,DEFAULT_TIMEOUT) ;

	if (m_close_base_time)
		nTimeout = std::min(nTimeout, mod_minus(*m_close_base_time, now));

	if (m_t_ack)
		nTimeout = std::min(nTimeout, mod_minus(m_t_ack + ACK_DELAY, now));

	if (m_rto_base) 
		nTimeout = std::min(nTimeout, mod_minus(m_rto_base + m_rx_rto, now));

	if (m_snd_wnd == 0)
		nTimeout = std::min(nTimeout, mod_minus(m_lastsend + m_rx_rto, now));

	if (m_state == TCP_ESTABLISHED&&m_slist.empty()&&m_retrans_slist.empty())
		nTimeout = std::min(nTimeout, mod_minus(m_lasttraffic + m_ping_interval, now));

	if(!m_unreliable_slist.empty())
		nTimeout = std::min(nTimeout, mod_minus(m_unreliable_slist.front().timeout,now));

	if (nTimeout<0)
		nTimeout=0;
	return true;
}

bool urdp_flow::__process(safe_buffer& buffer,const endpoint_type& from) 
{
	if (m_state == TCP_CLOSED) 
		return false;

	BOOST_ASSERT(m_self_holder&&m_self_holder.get()==this);

	uint32_t now = NOW();
	{
		if (buffer.length()<unreliable_packet_format_type::packet_size())
			return false;
		packet_reader<unreliable_packet_format_type> urdp_header(buffer);
		unreliable_packet_format_type& h=urdp_header.packet_format_def();

		//���convno��remote_endpoint,��������
		if (m_state>=TCP_SYN_SENT)
		{
			if (h.get_session_id()!= m_session_id
				||m_state>=TCP_ESTABLISHED&&m_remote_endpoint!=from
				) 
				return false;
		}

		m_lasttraffic = m_lastrecv = now;
		m_snd_wnd = h.get_window();
		in_speed_meter_+=buffer.length();

		//calculate lost rate
		wrappable_integer<int8_t> idForLostDetect=h.get_id_for_lost_detect();
		__calc_remote_to_local_lost_rate(&idForLostDetect);
		if (local_to_remote_lost_rate_<0)
		{
			local_to_remote_lost_rate_=h.get_lostrate_recving()*LOST_RATE_PRECISION;
		}
		else
		{
			double lostRate=h.get_lostrate_recving()*LOST_RATE_PRECISION;
			double a=out_speed_meter_.bytes_per_second()/(s_local_to_remote_speed_meter().bytes_per_second()+1.0);
			if (a>1.0)
				a=1.0;
			double oldRate=s_local_to_remote_lost_rate();
			double newRate=(oldRate*(1.0-a)+lostRate*a);
			s_local_to_remote_lost_rate()=oldRate*0.125+newRate*0.875;
		}

		//����Ƿ���unreliable msg
		if (h.get_control()==CTRL_UNRELIABLE_DATA
			||h.get_control()==CTRL_SEMIRELIABLE_DATA
			)
		{
			if (buffer.length()<unreliable_packet_format_type::packet_size()+4)//packet chunk header is 4
				return false;
			if (m_state == TCP_ESTABLISHED&&m_socket)
			{
				safe_buffer pkt=buffer.buffer_ref(unreliable_packet_format_type::packet_size());
				safe_buffer_io io(&pkt);
				uint16_t pktID;
				io>>pktID;
				if (!m_unreliable_rkeeper.try_keep(pktID,seconds(5)))
					return true;//�Ѿ��յ��������
				m_unreliable_rlist.push_back(pkt);
				__allert_readable();
				return true;
			}
			else
				return  false;
		}
	}

	if (buffer.length()<reliable_packet_format_type::packet_size())
		return false;

	//��������ϸ����
	packet_reader<reliable_packet_format_type> urdp_header(buffer);
	reliable_packet_format_type& h=urdp_header.packet_format_def();
	uint32_t len=(uint32_t)buffer.size();
	uint32_t rcvdDataLen=len-reliable_packet_format_type::packet_size();
	const char* data=buffer_cast<char*>(buffer)+reliable_packet_format_type::packet_size();

	bool notifyConnected=false;
	//bool notifyDisconnected=false;
	bool notifyWritable=false;
	bool notifyReadable=false;
	bool notifyAccepet=false;
	bool bConnect = false;
	bool shouldImediateAck=false;

	uint32_t seqno=h.get_seqno();
	uint32_t ackno=h.get_ackno();

	switch (h.get_control())
	{
	case CTRL_NETWORK_UNREACHABLE:
		if (m_state == TCP_SYN_SENT&&h.get_session_id()==m_session_id)
			__allert_connected(asio::error::host_unreachable);
		return true;
	case CTRL_CONNECT:
		bConnect = true;
		if (m_state == TCP_LISTEN) 
		{
			m_state = TCP_SYN_RECEIVED;

			//��¼�±�Ҫ�Ĳ���
			m_remote_peer_id=h.get_peer_id();
			m_session_id=h.get_session_id();
			m_ts_lastack=m_rcv_nxt=seqno+rcvdDataLen;//ֱ�ӽ�m_ts_lastack��ֵΪseqno+rcvdDataLen
			m_remote_endpoint=from;
			m_ts_recent =h.get_time_sending();
			m_ts_recent_now=now;
			m_lasttraffic = m_lastrecv = now;
			//��ACK�ŵ����Ͷ����У����̷���
			char myPeerID[4];
			char* pMyPeerID=myPeerID;
			write_int32(m_token->flow_id,pMyPeerID);
			__queue(myPeerID,4,CTRL_CONNECT_ACK);
			__attempt_send(sfImmediateAck);
			return true;//����ҪҲ��Ҫ�������´���
		}
		return true;//����ҪҲ��Ҫ�������´���
	case CTRL_CONNECT_ACK:
		if (m_state == TCP_SYN_SENT) 
		{
			if (h.get_session_id()!=m_session_id)
				return false;
			if (rcvdDataLen<4)
				return false;
			const char* pHisPeerID=data;
			m_remote_peer_id=read_uint32(data);
			m_state = TCP_ESTABLISHED;
			m_ts_lastack=m_rcv_nxt=seqno;//��m_ts_lastack��ֵΪseqno,���ں�������
			notifyConnected=true;// !!
			shouldImediateAck=true;
		}
		break;

	case CTRL_PUNCH:
		m_remote_endpoint=from;
		m_lasttraffic = m_lastrecv = now;
		if (m_retrans_slist.size()==1)//must be request pkt
		{
			SSegment& seg=m_retrans_slist.front();
			if (seg.ctrlType==CTRL_CONNECT)
			{
				seg.xmit=1;//set to 1
				__transmit(m_retrans_slist.begin(),now);//send conn request impdiatelly
			}
		}
		return true;

	case CTRL_RST:
		m_dissconnect_reason|=DISCONN_REMOTE;
		__allert_disconnected();
		return true;
	case CTRL_FIN:
		m_dissconnect_reason|=DISCONN_REMOTE;
		__packet_reliable_and_sendout(m_snd_nxt,CTRL_FIN_ACK,NULL,0);
		__allert_disconnected();
		return true;
	case CTRL_FIN_ACK:
		__allert_disconnected();
		return true;

	case CTRL_DATA:
	case CTRL_ACK:
		break;

	default:
		BOOST_ASSERT(0);
		break;
	}

	if (m_state>=TCP_SYN_SENT)
	{
		if (m_remote_endpoint!=from)
			return false;//drop it
	}
	m_lasttraffic = m_lastrecv = now;

	// Update timestamp //seqno<=ACK<seq+dataLen
	if (mod_less_equal(seqno,m_ts_lastack) 
		&& mod_less(m_ts_lastack, seqno+ rcvdDataLen)
		) 
	{
		m_ts_recent_now=now;
		m_ts_recent =h.get_time_sending() ;
	}

	if (m_shutdown==SD_GRACEFUL&&ackno==m_snd_nxt&&!m_close_base_time)
	{
		BOOST_ASSERT(m_socket==NULL);
		m_state=TCP_CLOSING;
		m_close_base_time=NOW()+DEFAULT_TIMEOUT;
		__packet_reliable_and_sendout(m_snd_nxt,CTRL_FIN,NULL,0);
		return false;
	}	

	//m_snd_una<h.ackno<=m_snd_nxt
	if (mod_less(m_snd_una,ackno)&&mod_less_equal(ackno,m_snd_nxt)) 
	{		
		// Calculate round-trip time
		if (h.get_time_echo()) 
		{
			long rtt = mod_minus((uint16_t)now, (uint16_t)h.get_time_echo());
			__updata_rtt(rtt);
		}

		m_snd_wnd = h.get_window();
		uint32_t nAcked =mod_minus(ackno,m_snd_una);
		m_snd_una = ackno;
		m_slen -= nAcked;
		BOOST_ASSERT(mod_less_equal(m_snd_una,m_snd_nxt));
		m_rto_base = (m_snd_una == m_snd_nxt) ? 0 : (now?now:1);

		//printf("acked:------------------------------------------:%d\n",m_snd_una);

		while (!m_retrans_slist.empty())
		{
			long eraseLen=mod_minus(m_snd_una,m_retrans_slist.front().seq);
			BOOST_ASSERT(eraseLen>=0);
			if (eraseLen>0)
				m_retrans_slist.pop_front();
			else
				break;
		}
		BOOST_ASSERT(m_retrans_slist.empty()||m_retrans_slist.front().seq==m_snd_una);

		//�����ش�����ٻָ��㷨
		/*
		1)���յ��������ظ���ACKʱ����ssthresh= max(FlightSize/2, 2*SMSS)��
		�ش���ʧ�ı��ĶΡ�����cwndΪssthresh����3���ı��Ķδ�С�����յ�3����
		��ack��˵���Ѿ����������Ķ��뿪�����ˣ�

		2)����ȷ�ϣ�ÿ���յ���һ���ظ���ACKʱ��cwnd����1�����Ķδ�С���յ�һ
		��ack˵��һ�����Ķ��뿪�����ˣ���������1������(���µ�cwnd������)��

		3)��ȫȷ�ϣ���һ�����ظ�ȷ�ϵ���ʱ�����ACKӦ�����ڽ����ش����һ��
		����ʱ���ڶԲ���1���ش���ȷ�ϣ�Ҳ�ǶԶ�ʧ�ķ�����յ��ĵ�1���ظ���
		ACK֮��������м䱨�Ķε�ȷ�ϣ�����cwnd=ssthresh��ssthreshΪ�ڵ�1��
		�����õ�ֵ��������ӵ�������㷨��
		*/
		if (m_dup_acks >= 3) 
		{
			if (m_snd_una >= m_recover) 
			{ 
				//��ȫȷ�ϣ�exit recovery��
				//��ACKȷ�����������ݵ����к�����recover��¼�����кţ�
				//���ACKȷ���������м䶪ʧ�����ݰ�����ʱ����cwnd����Ϊ
				//min(ssthresh, FlightSize + SMSS)������ssthresh ����Сcwnd,
				//�������ٻָ���
				long nInFlight =mod_minus(m_snd_nxt,m_snd_una);
				BOOST_ASSERT(nInFlight>=0);
				m_cwnd = std::min(m_ssthresh, (uint32_t)nInFlight + m_mss); // (Fast Retransmit) 
				m_dup_acks = 0;
			} 
			else 
			{
				//����ȷ�ϡ�
				//������ACK��ȷ�����в���������recover�������ݵĻ���Ϊ����ȷ�ϡ�
				//�ڴ�������£��ش���һ��û��ȷ�ϵ����ݶΣ�����ȷ�ϵ�������������Сӵ�����ڡ�
				//������"����ȷ��"ȷ��������һ��MSS�������ݣ���ӻ�һ��MSS��
				//���cwnd����ֵ����Ļ�������һ�������ݶΡ�
				//���"���ִ�������"��ͼȷ�������ٻָ����ս���ʱ����Լssthresh���������ݻ����������д��͡�
				//������²��˳����ٻָ����̡����ڿ��ٻָ��ڼ��һ������Ĳ��� ACK��ҲҪ�����ش���ʱ����  
				//recovery retransmit
				if (!__transmit(m_retrans_slist.begin(), now)) 
				{
					//std::cout << "recovery retransmit"<<m_retransList.begin()->seq<<std::endl;;
					__allert_disconnected();
					return false;
				}
				m_cwnd += m_mss - std::min(nAcked, m_cwnd);
			}
		} 
		else 
		{
			//����ӵ�������������״̬
			// Slow start, congestion avoidance
			m_dup_acks = 0;
			if (m_cwnd < m_ssthresh)
				m_cwnd += m_mss;
			else
				m_cwnd += (uint32_t)std::max((uint64)1, (uint64)m_mss * (uint64)m_mss / m_cwnd);
		}

		if ((m_state == TCP_SYN_RECEIVED) && !bConnect) 
		{
			m_state = TCP_ESTABLISHED;
			notifyAccepet=true;
		}

		// If we make room in the _send queue, notify the user
		// The goal it to make sure we always have at least enough data to fill the
		// window.  We'd like to notify the app when we are halfway to that point.
		const uint32_t kIdealRefillSize = (SND_BUF_SIZE + RECV_BUF_SIZE) / 2;
		if (m_detect_writable && (m_slen < kIdealRefillSize)) 
		{
			//m_detect_writable = false;
			notifyWritable=true;
			//if (m_connection)
			//	m_connection->on_writeable(this);
			//notify(evWrite);
		}
	} 
	else if (ackno== m_snd_una) //!(m_snd_una<h.ackno<=m_snd_nxt)
	{
		// !?! Note, tcp says don't do this... but otherwise how does a closed window become open?
		m_snd_wnd =h.get_window();

		// Check duplicate acks
		if (rcvdDataLen > 0) 
		{
			// it's a dup ack, but with a data payload, so don't modify m_dup_acks
		} 
		else if (m_snd_una != m_snd_nxt) 
		{

			m_dup_acks += 1;
			//��3���ظ���ACKȷ�ϵ���
			if (m_dup_acks == 3) 
			{ 
				//��������ش�(Fast Retransmit)
				/*������������ֵssthreshΪ:ssthresh = max(FlightSize / 2, 2*SMSS)    
				����FlightSize��ʾ�Ѿ����͵���û�б�ȷ�ϵ�������Ȼ�󽫷��͵�
				������к�ֵm_snd_nxt������recover�����У��ش���ʧ�İ�,
				Ȼ������ӵ������cwnd=ssthresh + 3*SMSS������ӵ�����ڡ�*/
				uint32_t nInFlight = m_snd_nxt - m_snd_una;
				m_ssthresh = std::max((nInFlight*3)/4, 2*m_mss);//m_ssthresh = std::max(nInFlight / 2, 2 * m_mss);
				m_recover = m_snd_nxt;
				BOOST_ASSERT(!m_retrans_slist.empty());
				if (!__transmit(m_retrans_slist.begin(), now)) 
				{
					__allert_disconnected();
					return false;
				}
			} 
			else if (m_dup_acks > 3)
			{
				//���ٻָ�(Fast Recover)
				//�ڿ��ٻָ��׶Σ����������յ���ÿ���ظ���ACK����ӵ�����ڵ���SMSS������ӵ�����ڣ�
				//ÿ�յ�һ���ظ�ACK˵��һ�����Ѿ��뿪���������Է���
				m_cwnd += m_mss;
			}
		} 
		else //!(m_snd_una != m_snd_nxt) 
		{
			m_dup_acks = 0;
		}
	}

	// Conditions were acks must be sent:
	// 1) Segment is too old (they missed an ACK) (immediately)
	// 2) Segment is too new (we missed a segment) (immediately)
	// 3) Segment has data (so we need to ACK!) (delayed)
	// ... so the only time we don't need to ACK, is an empty segment that points to rcv_nxt!

	SendFlags sflags = sfNone;
	if (seqno != m_rcv_nxt||shouldImediateAck)
		sflags = sfImmediateAck; // (Fast Recovery)
	else if (rcvdDataLen != 0)
		sflags = sfDelayedAck;

	// Adjust the incoming segment to fit our receive buffer
	if (seqno < m_rcv_nxt) 
	{
		uint32_t nAdjust = m_rcv_nxt - seqno;
		if (nAdjust < rcvdDataLen) 
		{
			seqno += nAdjust;
			data += nAdjust;
			rcvdDataLen -= nAdjust;
		} 
		else 
		{
			rcvdDataLen = 0;
			seqno=m_rcv_nxt;
		}
	}

	//������չ��������ٽ��գ����������ν��պ�m_rlen����kRcvBufSize��
	//��ˣ�������Ҫ�Խ��ճ����ϸ���kRcvBufSize���е���
	if (!m_rlist.empty())
	{
		if (mod_minus(seqno+rcvdDataLen,m_rcv_nxt)>=RECV_BUF_SIZE)
			rcvdDataLen=0;
	}

	bool bIgnoreData = (h.get_control()!=CTRL_DATA) || (m_shutdown != SD_NONE);
	bool bNewData = false;

	if (rcvdDataLen > 0) 
	{
		if (bIgnoreData) 
		{
			if (seqno== m_rcv_nxt) 
				m_rcv_nxt += rcvdDataLen;
		} 
		else 
		{
			//uint32_t nOffset = seqno - m_rcv_nxt;
			//memcpy(m_rbuf + m_rlen + nOffset, seg.data, seg.len);
			//std::cout<<" seqno------:"<< seqno<<"      m_rcv_nxt:"<<m_rcv_nxt<<std::endl;
			RSegment rseg;
			rseg.seq=seqno;
			safe_buffer_io io(&(rseg.buf));
			io.write(data,rcvdDataLen);
			std::pair<RSegmentList::iterator,bool >insertRst=m_rlist.insert(rseg);

			/*		if (m_rlist.size()>15)
			{
			bool t=m_detect_readable;
			std::cout<<"sd"<<std::endl;
			}*/

			if (insertRst.second)
			{
				if (m_rcv_wnd<rcvdDataLen)
					m_rcv_wnd=0;
				else
					m_rcv_wnd -= rcvdDataLen;
			}

			if (seqno == m_rcv_nxt) 
			{
				BOOST_ASSERT(insertRst.second);
				//m_rlen += rcvdDataLen;
				//m_rcv_nxt += rcvdDataLen;
				//m_rcv_wnd -= rcvdDataLen;
				bNewData = true;

				RSegmentList::iterator it = m_rlist.begin();
				while ((it != m_rlist.end()) && mod_less_equal(it->seq , m_rcv_nxt)) 
				{
					if (mod_less(m_rcv_nxt,it->seq + it->buf.size()))
					{
						sflags = sfImmediateAck; // (Fast Recovery)
						uint32_t nAdjust 
							=mod_minus((it->seq + it->buf.size()),m_rcv_nxt);
						m_rlen += nAdjust;
						m_rcv_nxt += nAdjust;
					}
					++it;
				}
			} 
		}
	}

	__attempt_send(sflags);

	// If we have new data, notify the user
	if (bNewData && m_detect_readable) {
		//m_detect_readable = false;
		notifyReadable=true;
		/*	if (m_connection) {
		m_connection->on_readable(this);
		}*/
		//notify(evRead);
	}

	if (notifyConnected)
	{
		__allert_connected(error_code());
	}
	if (notifyWritable)
	{
		__allert_writeable();
	}
	if (notifyReadable)
	{
		__allert_readable();
	}
	if (notifyAccepet)
	{
		__allert_accepted();
	}

	/*
	if (notifyWritable&&m_socket&&m_detect_writable&&m_slen<SND_BUF_SIZE)
	{
	m_detect_writable=false;
	m_socket->on_writeable();
	}
	if (notifyReadable&&m_socket&&m_detect_readable&&_can_let_transport_read())
	{
	m_detect_readable=false;
	m_socket->on_readable();
	}
	if (notifyAccepet)
	{
	boost::shared_ptr<acceptor_type> acc=m_acceptor.lock();
	if (acc)
	{
	acc->accept_flow(SHARED_OBJ_FROM_THIS);
	}
	}*/

	return true;
}

void urdp_flow::__allert_connected(const error_code & ec)
{
	m_establish_time=NOW();
	m_close_base_time.reset();
	BOOST_ASSERT(m_is_active);
	if (ec||!m_socket)
	{
		__to_closed_state();
	}
	else
	{
		keep_async_receiving();
		m_socket->on_connected(ec);
	}
}

void urdp_flow::__allert_disconnected()
{
	if (is_canceled_op(op_stamp())||!m_socket)
	{
		__to_closed_state();
		return;
	}
	if (m_socket&&(m_dissconnect_reason&DISCONN_LOCAL)==0&&m_state!=TCP_CLOSED)
	{
		error_code ec=(m_dissconnect_reason&DISCONN_ERROR)==DISCONN_ERROR?
			asio::error::connection_aborted : asio::error::connection_reset;
		if (m_state>=TCP_ESTABLISHED)
			m_socket->on_disconnected(ec);
		else if(m_is_active)
			m_socket->on_connected(ec);
	}
	__to_closed_state();
}

void urdp_flow::__allert_readable()
{
	if (is_canceled_op(op_stamp())||!m_socket)
	{
		__to_closed_state();
		return;
	}
	else if (m_detect_readable&&__can_let_transport_read())
	{
		m_detect_readable=false;
		if (b_keep_recving_)
			__do_async_receive(op_stamp());
	}
}

void urdp_flow::__allert_writeable()
{
	if (is_canceled_op(op_stamp())||!m_socket)
	{
		__to_closed_state();
		return;
	}
	else if (m_slen<SND_BUF_SIZE)
	{
		m_detect_writable=false;
		m_socket->on_writeable();
	}
}

void urdp_flow::__allert_accepted()
{
	if (is_canceled_op(op_stamp()))
	{
		__to_closed_state();
		return;
	}

	m_establish_time=NOW();
	m_close_base_time.reset();
	acceptor_sptr acc(m_acceptor.lock());
	if (acc)
	{
		keep_async_receiving();
		acc->accept_flow(SHARED_OBJ_FROM_THIS);
	}
	else
		close();
}

void urdp_flow::__allert_received(safe_buffer buf)
{
	if (is_canceled_op(op_stamp())||!m_socket)
	{
		__to_closed_state();
		return;
	}
	else
	{
		m_socket->on_received(buf);
	}
}

void 
urdp_flow::__updata_rtt(long rtt)
{
	if (rtt >= 0) 
	{
		if (m_rx_srtt == 0) 
		{
			m_rx_srtt = rtt;
			m_rx_rttvar = rtt / 2;
		} 
		else 
		{
			m_rx_rttvar = (3 * m_rx_rttvar + abs(long(rtt - m_rx_srtt))) / 4;
			m_rx_srtt = (7 * m_rx_srtt + rtt) / 8;
		}
		m_rx_rto = bound(MIN_RTO, m_rx_srtt + std::max((uint32_t)1, 4 * m_rx_rttvar), MAX_RTO);
#if 0
		std::cout<<"cwnd: "<<m_cwnd 
			<< ", rtt: " << rtt
			<< ", srtt: " << m_rx_srtt
			<< ", rto: " << m_rx_rto<<std::endl;
#endif // _DEBUGMSG
	} 
	else 
	{
		//std::cout<<rtt<<std::endl;
		//BOOST_ASSERT(false);
	}
}


bool
urdp_flow::__transmit(const SSegmentList::iterator& itr, uint32_t now)
{
	SSegment& seg=*itr;

	if (seg.xmit >= ((m_state < TCP_ESTABLISHED)?10:8)
		&&mod_minus(now, m_lastrecv) >= 10000
		)
	{
		//̫���ط�ʧ�ܣ���Ϊ�Է������ˣ����ط���ʧ��
		return false;
	}

	uint32_t maxTransLen = std::min((uint32_t)seg.buf.size(), m_mss);
	BOOST_ASSERT(maxTransLen==(uint32_t)seg.buf.size());
	__packet_reliable_and_sendout(seg.seq,seg.ctrlType,buffer_cast<char*>(seg.buf),maxTransLen);

	if (seg.xmit == 0) 
		m_snd_nxt += maxTransLen;
	seg.xmit += 1;

	//һ��Ƭ�η�������ʱ��δ�յ�ack����Ӧ�������ط���ʶͬʱ��¼���η���ʱ��
	if (m_rto_base == 0)
		m_rto_base =(now?now:1);

	return true;
}

void
urdp_flow::__attempt_send(SendFlags sflags) 
{
	uint32_t now = NOW();

	if (mod_minus(now, m_lastsend) > static_cast<long>(m_rx_rto))
		m_cwnd =std::max(2*m_mss,m_cwnd/2);

	while (true)
	{
		uint32_t cwnd = m_cwnd;
		if ((m_dup_acks == 1) || (m_dup_acks == 2)) 
			cwnd += m_dup_acks * m_mss;// Limited Transmit
		uint32_t nWindow = std::min(m_snd_wnd, cwnd);
		uint32_t nInFlight =mod_minus(m_snd_nxt,m_snd_una);
		uint32_t nUseable = (nInFlight < nWindow) ? (nWindow - nInFlight) : 0;

		uint32_t nAvailable = std::min(m_slen - nInFlight, m_mss);

		if (nAvailable > nUseable) 
		{
			if (nUseable * 4 < nWindow) 
				nAvailable = 0;// RFC 813 - avoid SWS
			else 
				nAvailable = nUseable;
		}

		if (nAvailable == 0) 
		{
			if (sflags == sfNone)
				return;

			// If this is an immediate ack, or the second delayed ack
			if ((sflags == sfImmediateAck) || m_t_ack) 
				__packet_reliable_and_sendout(m_snd_nxt,CTRL_ACK,NULL,0);
			else 
				m_t_ack = (now?now:1);
			return;       
		}

		// Nagle algorithm
		if (mod_less(m_snd_una,m_snd_nxt) && (nAvailable < m_mss))
			return;

		// Find the next segment to transmit
		BOOST_ASSERT(!m_slist.empty());
		if (!__transmit(m_slist.begin(), now)) 
			return;// TODO: consider closing socket
		m_retrans_slist.push_back(m_slist.front());
		m_slist.pop_front();
		sflags = sfNone;
	}
}

void
urdp_flow::__to_closed_state() 
{
	if (m_state==TCP_CLOSED)
	{
		BOOST_ASSERT(!m_self_holder);
		return ;
	}

	set_cancel();
	m_state=TCP_CLOSED;
	if (m_token)
		m_token.reset();
	if (m_timer)
		m_timer->cancel();
	m_socket=NULL;
	m_acceptor.reset();

	//To close state we must reset self_holder_, otherwise, we cant delete this_ptr
	//because of the "cycles of shared_ptr". 
	//But, if we just self_holder_ here, and the urdp reset shared_ptr of this_ptr,
	//then, this_ptr will be deleted at once. Unfortunately, this function may be
	//called by another function of this_type, so, Crash!
	//Post is the right way. 
	if (m_self_holder)
	{
		get_io_service().post(
			boost::bind(&this_type::__do_nothing,SHARED_OBJ_FROM_THIS)
			);
		m_self_holder.reset();//this_ptr will not be deleted , because we post a shared_ptr of this
	}
}

void urdp_flow::__incress_rto()
{
	// Back off retransmit timer.  Note: the limit is lower when connecting.
	uint32_t rto_limit = (m_state < TCP_ESTABLISHED) ? DEF_RTO : MAX_RTO;
	m_rx_rto = std::min(rto_limit, m_rx_rto * 2);//ÿ���ط�����Ҫ���ط�ʱ���ӳ�
	if (m_rx_srtt>0)
	{//ʹ��urdp����ͨtcp������
		if (m_rx_rto>8*m_rx_srtt&&m_rx_rto>MIN_RTO)
			m_rx_rto=std::min(8*m_rx_srtt,MAX_RTO);
	}
	if(m_rx_rto<MIN_RTO)
		m_rx_rto=MIN_RTO;
}


double urdp_flow::__calc_remote_to_local_lost_rate(wrappable_integer<int8_t>* id) const
{
	if (!m_token)
		return 1.0;

	uint32_t now=NOW();
	std::map<wrappable_integer<int8_t>,uint32_t>& qm=recvd_seqno_mark_for_lost_rate_;
	while(qm.size()>=120
		||qm.size()>0&&mod_less(qm.begin()->second+3000,now)
		||qm.size()>0&&id&&qm.begin()->first>*id&&qm.rbegin()->first<*id
		)//wrappable_integer<int8_t>�����׷���A>B B>C ��A<C����������Ҫ��ֹ����
	{
		qm.erase(qm.begin());
	}
	if (id)
	{
		qm.insert(std::make_pair(*id,now));
	}
	if (!qm.empty())
	{
		double cnt=int(qm.rbegin()->first-qm.begin()->first)+1;
		double lostRate=1.0-(double)(qm.size())/cnt;
		if (remote_to_local_lost_rate_<0)
			remote_to_local_lost_rate_=lostRate;
		else
			remote_to_local_lost_rate_=remote_to_local_lost_rate_*0.125+lostRate*0.875;
	}
	else
	{
		remote_to_local_lost_rate_=0;
	}

	double a=in_speed_meter_.bytes_per_second()/(s_remote_to_local_speed_meter().bytes_per_second()+1.0);
	if (a>1.0) a=1.0;
	double oldRate=s_remote_to_local_lost_rate();
	double newRate=(oldRate*(1.0-a)+remote_to_local_lost_rate_*a);
	s_remote_to_local_lost_rate()=oldRate*0.125+newRate*0.875;

	return remote_to_local_lost_rate_;
}

/*
int urdp_flow::__try_transmit_unreliable(safe_buffer& data, uint8 control, 
uint16_t msgType)
{
if (data.size()>m_mss||data.size()==0)
return data.size();//������ȳ��ȴ���mss����ֱ�Ӷ�������

BOOST_ASSERT(control==CTRL_SEMIRELIABLE_DATA||control==CTRL_UNRELIABLE_DATA);
unreliable_snd_next_++;
msec_type now=timer_type::time_traits_type::now_tick_count();
bool conjest=!slow_start_
&&out_speed_meter_.bytes_per_second()>=1.05*bandwidth_thresh_;
if(conjest
||control==CTRL_SEMIRELIABLE_DATA
)//no reliable packet, but conjest, we have to persist it for a shot time
{
snd_eliment elm;
if (srtt_>0)
elm.out_time=now+srtt_/3;
else
elm.out_time=now+100;
elm.control=control;
elm.xmit=0;
elm.max_xmit=1;
elm.data=data;
elm.msg_type=msgType;
elm.seqno=unreliable_snd_next_;
unreliable_snd_que_.push_back(elm);
//waiting for a little while to trans these packet
msec_type t=msec_type((double)(SMSS)/(double)bandwidth_thresh_ * 1000);
t=std::max(t,30);
if (timer_->is_idle()
||total_milliseconds(timer_->expires_from_now())>(t+10)//����10ms���
)
{
error_code ec;
timer_->async_wait(millisec(t));
BOOST_ASSERT(timer_->time_signal().size()==1);
}
if (control==CTRL_UNRELIABLE_DATA)
return;
else
{
//����semireliable�İ��������ǲ�����ӵ�������������ȷ���һ��;
//(���ϳ�ʱ�����һ�η��ͣ������Σ���������ý���еġ��������󡱰���
//��ʵʱ�ԺͿɿ��Զ���һ��Ҫ������Ρ�
__packet_and_transmit(unreliable_snd_next_,control,buffer_cast<char*>(data),
data.length(),msgType);
}
}
else
{
__packet_and_transmit(unreliable_snd_next_,control,buffer_cast<char*>(data),
data.length(),msgType);
}
}
*/
NAMESPACE_END(urdp)
NAMESPACE_END(p2engine)

#if defined(_MSC_VER)&&_MSC_VER<=1500
# pragma warning (pop)
#endif
#include <unistd.h>
#include <string>
#include <assert.h>
#include <list>
#include <stdio.h>
#include <queue>
#include <limits.h>
#include <signal.h>

#include "select.h"
#include "timestamp.h"
#include "packetsocket.hh"

using namespace std;

bool end_loop = false;
void sigfunc(int signum)
{
    signum += 0;
    end_loop = true;
}


class DelayQueue
{
private:
  class DelayedPacket
  {
  public:
    uint64_t entry_time;
    uint64_t release_time;
    string contents;

    DelayedPacket( uint64_t s_e, uint64_t s_r, const string & s_c )
      : entry_time( s_e ), release_time( s_r ), contents( s_c ) {}
  };

  class PartialPacket
  {
  public:
    int bytes_earned;
    DelayedPacket packet;

    PartialPacket( int s_b_e, const DelayedPacket & s_packet ) : bytes_earned( s_b_e ), packet( s_packet ) {}
  };

  static const int SERVICE_PACKET_SIZE = 1514;

  uint64_t convert_timestamp( const uint64_t absolute_timestamp ) const { return absolute_timestamp; }

  FILE * _output;
  const string _name;

  queue< DelayedPacket > _delay;
  queue< DelayedPacket > _pdp;
  queue< PartialPacket > _limbo;

  queue< uint64_t > _schedule;

  vector< string > _delivered;

  const uint64_t _ms_delay;
  const float _loss_rate;

  uint64_t _total_bytes;
  uint64_t _used_bytes;

  uint64_t _queued_bytes;
  uint64_t _bin_sec;

  uint64_t _base_timestamp;

  uint32_t _packets_added;
  uint32_t _packets_dropped;
  string _file_name;

  static const int queue_limit_in_packets = 256;

  void tick( void );

  /* forbid copies */
  DelayQueue( const DelayQueue & other ) = delete;
  DelayQueue & operator=( const DelayQueue & other ) = delete;

public:
  DelayQueue( FILE * s_output, const string & s_name, const uint64_t s_ms_delay, const string filename, const uint64_t base_timestamp, const float loss_rate );

  int wait_time( void );
  vector< string > read( void );
  void write( const string & packet );
  void schedule_from_file( const uint64_t base_timestamp );
};

DelayQueue::DelayQueue( FILE * s_output, const string & s_name, const uint64_t s_ms_delay, const string filename, const uint64_t base_timestamp, const float loss_rate )
  : _output( s_output ),
    _name( s_name ),
    _delay(),
    _pdp(),
    _limbo(),
    _schedule(),
    _delivered(),
    _ms_delay( s_ms_delay ),
    _loss_rate(loss_rate),
    _total_bytes( 0 ),
    _used_bytes( 0 ),
    _queued_bytes( 0 ),
    _bin_sec( base_timestamp / 1000 ),
    _base_timestamp( base_timestamp ),
    _packets_added ( 0 ),
    _packets_dropped( 0 ),
    _file_name( filename )
{
  /* Read schedule from file */
  schedule_from_file( base_timestamp );

  /* Initialize seed for probabilistic loss model */
  srand(0);
  fprintf( _output, "# Initialized %s queue with %d services.\n", filename.c_str(), (int)_schedule.size() );
  fprintf( _output, "# Direction: %s\n", _name.c_str() );
  fprintf( _output, "# base timestamp: %lu\n", base_timestamp );
}

void DelayQueue::schedule_from_file( const uint64_t base_timestamp )
{
  FILE *f = fopen( _file_name.c_str(), "r" );
  if ( f == NULL ) {
    perror( "fopen" );
    exit( 1 );
  }

  /* Only populate when the schedule is empty */
  assert( _schedule.empty() );

  while ( 1 ) {
    uint64_t ms;
    int num_matched = fscanf( f, "%lu\n", &ms );
    if ( num_matched != 1 ) {
      break;
    }

    ms += base_timestamp;

    if ( !_schedule.empty() ) {
      assert( ms >= _schedule.back() );
    }

    _schedule.push( ms );
  }
  fclose( f );
}

int DelayQueue::wait_time( void )
{
  int delay_wait = INT_MAX, schedule_wait = INT_MAX;

  uint64_t now = timestamp();

  tick();

  if ( !_delay.empty() ) {
    delay_wait = _delay.front().release_time - now;
    if ( delay_wait < 0 ) {
      delay_wait = 0;
    }
  }

  if ( !_schedule.empty() ) {
    schedule_wait = _schedule.front() - now;
    assert( schedule_wait >= 0 );
  }

  return min( delay_wait, schedule_wait );
}

vector< string > DelayQueue::read( void )
{
  tick();

  vector< string > ret( _delivered );
  _delivered.clear();

  return ret;
}

void DelayQueue::write( const string & packet )
{
  float r= rand()/(float)RAND_MAX;
  _packets_added++;
  if (r < _loss_rate) {
   _packets_dropped++;
   fprintf(stderr, "# %s , Stochastic drop of packet, _packets_added so far %d , _packets_dropped %d , drop rate %f \n",
                  _name.c_str(), _packets_added,_packets_dropped , (float)_packets_dropped/(float) _packets_added );
  }
  else {
    uint64_t now( timestamp() );

    if ( _delay.size() >= queue_limit_in_packets ) {
      fprintf( _output, "# %lu + %lu (dropped)\n",
	       convert_timestamp( now ),
	       packet.size() );

      return; /* drop the packet */
    }

    DelayedPacket p( now, now + _ms_delay, packet );
    _delay.push( p );

    fprintf( _output, "%lu + %lu\n",
	     convert_timestamp( now ),
	     packet.size() );

    _queued_bytes=_queued_bytes+packet.size();
  }
}

void DelayQueue::tick( void )
{
  uint64_t now = timestamp();

  /* If the schedule is empty, repopulate it */
  if ( _schedule.empty() ) {
    schedule_from_file( now );
  }

  /* move packets from end of delay to PDP */
  while ( (!_delay.empty())
	  && (_delay.front().release_time <= now) ) {
    _pdp.push( _delay.front() );
    _delay.pop();
  }

  /* execute packet delivery schedule */
  while ( (!_schedule.empty())
	  && (_schedule.front() <= now) ) {
    /* grab a PDO */
    const uint64_t pdo_time = _schedule.front();
    _schedule.pop();
    /* delivery opportunity */
    fprintf( _output, "%lu # %d\n", convert_timestamp( pdo_time ), SERVICE_PACKET_SIZE );

    int bytes_to_play_with = SERVICE_PACKET_SIZE;

    /* execute limbo queue first */
    if ( !_limbo.empty() ) {
      if ( _limbo.front().bytes_earned + bytes_to_play_with >= (int)_limbo.front().packet.contents.size() ) {
	/* deliver packet */
	_total_bytes += _limbo.front().packet.contents.size();
	_used_bytes += _limbo.front().packet.contents.size();

	/*
	if ( _printing ) {
	  printf( "%s %lu delivery %d %lu leftover\n", _name.c_str(), convert_timestamp( pdo_time ), int(pdo_time - _limbo.front().packet.entry_time), _limbo.front().packet.contents.size() );
	}
	*/
	/* new-style output (mahimahi-style) */
	fprintf( _output, "%lu - %lu %d\n",
		 convert_timestamp( pdo_time ),
		 _limbo.front().packet.contents.size(),
		 int(pdo_time - _limbo.front().packet.entry_time) );

	_delivered.push_back( _limbo.front().packet.contents );
	bytes_to_play_with -= (_limbo.front().packet.contents.size() - _limbo.front().bytes_earned);
	assert( bytes_to_play_with >= 0 );
	_limbo.pop();
	assert( _limbo.empty() );
      } else {
	_limbo.front().bytes_earned += bytes_to_play_with;
	bytes_to_play_with = 0;
	assert( _limbo.front().bytes_earned < (int)_limbo.front().packet.contents.size() );
      }
    }

    /* execute regular queue */
    while ( bytes_to_play_with > 0 ) {
      assert( _limbo.empty() );

      /* will this be an underflow? */
      if ( _pdp.empty() ) {
	/* underflow */
	/*
	if ( _printing ) {
	  printf( "%s %lu underflow %d\n", _name.c_str(), convert_timestamp( pdo_time ), bytes_to_play_with );
	}
	*/
	_total_bytes += bytes_to_play_with;
	bytes_to_play_with = 0;
      } else {
	/* dequeue whole and/or partial packet */
	DelayedPacket packet = _pdp.front();
	_pdp.pop();
	if ( bytes_to_play_with >= (int)packet.contents.size() ) {
	  /* deliver whole packet */
	  _total_bytes += packet.contents.size();
	  _used_bytes += packet.contents.size();

	  /*
	  if ( _printing ) {
	    printf( "%s %lu delivery %d %lu\n", _name.c_str(), convert_timestamp( pdo_time ), int(pdo_time - packet.entry_time), packet.contents.size() );
	  }
	  */

	  /* new-style output (mahimahi-style) */
	  fprintf( _output, "%lu - %lu %d\n",
		   convert_timestamp( pdo_time ),
		   packet.contents.size(),
		   int(pdo_time - packet.entry_time) );

	  _delivered.push_back( packet.contents );
	  bytes_to_play_with -= packet.contents.size();
	} else {
	  /* put packet in limbo */
	  assert( _limbo.empty() );

	  assert( bytes_to_play_with < (int)packet.contents.size() );

	  PartialPacket limbo_packet( bytes_to_play_with, packet );

	  _limbo.push( limbo_packet );
	  bytes_to_play_with -= _limbo.front().bytes_earned;
	  assert( bytes_to_play_with == 0 );
	}
      }
    }
  }

  while ( now / 1000 > _bin_sec ) {
    //    fprintf( stderr, "%s %ld %ld / %ld = %.1f %% %ld \n", _name.c_str(), _bin_sec - (_base_timestamp / 1000), _used_bytes, _total_bytes, 100.0 * _used_bytes / (double) _total_bytes , _queued_bytes );
    _total_bytes = 0;
    _used_bytes = 0;
    _queued_bytes = 0;
    _bin_sec++;
  }
}

int main( int argc, char *argv[] )
{
  if ( argc != 11 ) {
    return EXIT_FAILURE;
  }

  signal(SIGINT, sigfunc);
  signal(SIGTERM, sigfunc);
  signal(SIGHUP, sigfunc);

  /* Usage
  cellsim [1]=>up_filename [2]=>down_filename
          [3]=>uplink_lossrate [4]=>downlink_lossrate
          [5]=>uplink_delay [6]=>downlink_delay
          [7]=>internet_side_interface [8]=>client_side_interface
          [9]=>uplink_log [10]=>downlink_log
  */

  const string up_filename = argv[ 1 ];
  const string down_filename = argv[ 2 ];

  const double uplink_loss_rate = atof( argv[ 3 ] );
  const double downlink_loss_rate = atof( argv[ 4 ] );

  const uint64_t uplink_delay = strtoull( argv[ 5 ], NULL, 0 );
  const uint64_t downlink_delay = strtoull( argv[ 6 ], NULL, 0 );

  const string internet_side_interface = argv[ 7 ];
  const string client_side_interface   = argv[ 8 ];

  FILE * up_output = fopen( argv[ 9 ], "w" );
  FILE * down_output = fopen( argv[ 10 ], "w" );

  PacketSocket internet_side( internet_side_interface );
  PacketSocket client_side( client_side_interface );

  /* Read in schedule */
  uint64_t now = timestamp();
  DelayQueue uplink( up_output, "uplink", uplink_delay, up_filename, now,
                     uplink_loss_rate );

  DelayQueue downlink( down_output, "downlink", downlink_delay, down_filename, now,
                       downlink_loss_rate );

  Select &sel = Select::get_instance();
  sel.add_fd( internet_side.fd() );
  sel.add_fd( client_side.fd() );

  while ( !end_loop ) {
    int wait_time = min( uplink.wait_time(), downlink.wait_time() );
    int active_fds = sel.select( wait_time );
    if ( active_fds < 0 ) {
      perror( "select" );
      exit( 1 );
    }

    if ( sel.read( client_side.fd() ) ) {
      for ( const auto & it : client_side.recv_raw() ) {
	uplink.write( it );
      }
    }

    if ( sel.read( internet_side.fd() ) ) {
      for ( const auto & it : internet_side.recv_raw() ) {
	downlink.write( it );
      }
    }

    for ( const auto & it : uplink.read() ) {
      internet_side.send_raw( it );
    }

    for ( const auto & it : downlink.read() ) {
      client_side.send_raw( it );
    }
  }

  fclose(up_output);
  fclose(down_output);
  exit(0);

}

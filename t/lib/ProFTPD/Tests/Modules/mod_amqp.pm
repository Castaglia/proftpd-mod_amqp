package ProFTPD::Tests::Modules::mod_amqp;

use lib qw(t/lib);
use base qw(ProFTPD::TestSuite::Child);
use strict;

use File::Path qw(mkpath);
use File::Spec;
use IO::Handle;

use ProFTPD::TestSuite::FTP;
use ProFTPD::TestSuite::Utils qw(:auth :config :features :running :test :testsuite);

$| = 1;

my $order = 0;

my $TESTS = {
  amqp_log_on_event => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  amqp_log_on_event_custom_routing_key => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  amqp_log_on_event_custom_exchange => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  amqp_log_on_event_per_dir => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  amqp_log_on_event_per_dir_none => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  amqp_log_on_event_using_tls => {
    order => ++$order,
    test_class => [qw(forking mod_tls)],
  },

  amqp_log_on_event_using_tls_verify => {
    order => ++$order,
    test_class => [qw(forking mod_tls)],
  },

  amqp_log_on_event_using_tls_verify_peer => {
    order => ++$order,
    test_class => [qw(forking mod_tls)],
  },

  amqp_log_on_event_using_tls_client_cert => {
    order => ++$order,
    test_class => [qw(forking mod_tls)],
  },

  amqp_opt_persistent_delivery => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  amqp_config_app_id => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  amqp_config_msg_type => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  amqp_config_msg_expires_before_expiry => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  amqp_config_msg_expires_after_expiry => {
    order => ++$order,
    test_class => [qw(forking)],
  },

  amqp_config_timeout => {
    order => ++$order,
    test_class => [qw(forking)],
  },

};

sub new {
  return shift()->SUPER::new(@_);
}

sub list_tests {
  # Check for the required Perl modules:
  #
  #  Net::AMQP::RabbitMQ

  my $required = [qw(
    JSON
    Net::AMQP::RabbitMQ
  )];

  foreach my $req (@$required) {
    eval "use $req";
    if ($@) {
      print STDERR "\nWARNING:\n + Module '$req' not found, skipping all tests\n";

      if ($ENV{TEST_VERBOSE}) {
        print STDERR "Unable to load $req: $@\n";
      }

      return qw(testsuite_empty_test);
    }
  }

  return testsuite_get_runnable_tests($TESTS);
}

sub get_rmq_host {
  my $rmq_host = 'localhost';

  if (defined($ENV{RABBITMQ_HOST})) {
    $rmq_host = $ENV{RABBITMQ_HOST};
  }

  return $rmq_host;
}

# Note: We only declare 'direct' exchanges for this testing currently
sub rmq_exchange_declare {
  my $name = shift;
  my $channel_id = 1;

  require Net::AMQP::RabbitMQ;

  my $mq = Net::AMQP::RabbitMQ->new();

  my $connect_opts = {
  };

  $mq->connect(get_rmq_host(), $connect_opts);
  $mq->channel_open($channel_id);

  my $exchange_opts = {
    exchange_type => 'direct',
  };

  $mq->exchange_declare($channel_id, $name, $exchange_opts);

  $mq->channel_close($channel_id);
  $mq->disconnect();
  return 1;
}

sub rmq_exchange_delete {
  my $name = shift;
  my $channel_id = 1;

  require Net::AMQP::RabbitMQ;

  my $mq = Net::AMQP::RabbitMQ->new();

  my $connect_opts = {
  };

  $mq->connect(get_rmq_host(), $connect_opts);
  $mq->channel_open($channel_id);

  my $exchange_opts = {
    if_unused => 0,
  };
  $mq->exchange_delete($channel_id, $name, $exchange_opts);

  $mq->channel_close($channel_id);
  $mq->disconnect();
  return 1;
}

sub rmq_queue_declare {
  my $name = shift;
  my $opts = shift;
  my $channel_id = 1;

  require Net::AMQP::RabbitMQ;

  my $mq = Net::AMQP::RabbitMQ->new();

  my $connect_opts = {
  };

  $mq->connect(get_rmq_host(), $connect_opts);
  $mq->channel_open($channel_id);

  my $queue_opts = {
    auto_delete => 0,
  };

  $mq->queue_declare($channel_id, $name, $queue_opts);

  if ($opts->{exchange} && $opts->{routing_key}) {
    $mq->queue_bind($channel_id, $name, $opts->{exchange},
      $opts->{routing_key});
  }

  $mq->channel_close($channel_id);
  $mq->disconnect();
  return 1;
}

sub rmq_queue_delete {
  my $name = shift;
  my $channel_id = 1;

  require Net::AMQP::RabbitMQ;

  my $mq = Net::AMQP::RabbitMQ->new();

  my $connect_opts = {
  };

  $mq->connect(get_rmq_host(), $connect_opts);
  $mq->channel_open($channel_id);

  my $queue_opts = {
    if_unused => 0,
    if_empty => 0,
  };
  $mq->queue_delete($channel_id, $name, $queue_opts);

  $mq->channel_close($channel_id);
  $mq->disconnect();
  return 1;
}

sub rmq_queue_getall {
  my $name = shift;
  my $channel_id = 1;

  require Net::AMQP::RabbitMQ;

  my $mq = Net::AMQP::RabbitMQ->new();

  my $connect_opts = {
  };

  $mq->connect(get_rmq_host(), $connect_opts);
  $mq->channel_open($channel_id);

  my $queue_opts = {
    if_unused => 0,
    if_empty => 0,
  };

  my $msgs = [];
  while (my $msg = $mq->get($channel_id, $name, $queue_opts)) {
    push(@$msgs, $msg);
  }

  $mq->channel_close($channel_id);
  $mq->disconnect();
  return $msgs;
}

# Tests

sub amqp_log_on_event {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5672",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

if (open(my $fh, "< $setup->{config_file}")) {
  while (my $line = <$fh>) {
    chomp($line);
    print STDERR "# $line\n";
  }
  close($fh);

} else {
  die("Can't open $setup->{config_file}: $!");
}

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 4 || $nrecords == 5,
      "Expected 4-5 records, got $nrecords");

    # Assert the message was published with the expected/configured properties
    my $content_type = $data->[0]->{props}->{content_type};
    $self->assert($content_type eq 'application/json',
      "Expected content type property 'application/json', got '$content_type'");

    my $type = $data->[0]->{props}->{type};
    $self->assert($type eq 'log',
      "Expected type property 'log', got '$type'");

    my $app_id = $data->[0]->{props}->{app_id};
    $self->assert($app_id eq 'proftpd',
      "Expected app ID property 'proftpd', got '$app_id'");

    my $ts = $data->[0]->{props}->{timestamp};
    $self->assert($ts > 0, "Expected timestamp property, got $ts");

    my $delivery_mode = $data->[0]->{props}->{delivery_mode};
    $self->assert($delivery_mode == 1,
      "Expected delivery mode property 1, got $delivery_mode");

    require JSON;
    my $json = $data->[3]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_log_on_event_custom_routing_key {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $routing_key = 'ftp.127.0.0.1';
  my $queue = $routing_key;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5672",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name routing ftp.%a",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 4 || $nrecords == 5,
      "Expected 4-5 records, got $nrecords");

    require JSON;
    my $json = $data->[3]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_log_on_event_custom_exchange {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';

  my $exchange = 'ftp.127.0.0.1';
  rmq_exchange_delete($exchange);
  rmq_exchange_declare($exchange);

  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue, {
    exchange => $exchange,
    routing_key => $fmt_name,
  });

  my $amqp_server = get_rmq_host();

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5672",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name exchange ftp.%a",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 4 || $nrecords == 5,
      "Expected 4-5 records, got $nrecords");

    require JSON;
    my $json = $data->[3]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  rmq_queue_delete($queue);
  rmq_exchange_delete($exchange);

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_log_on_event_per_dir {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $sub_dir = File::Spec->rel2abs("$tmpdir/test.d");
  mkpath($sub_dir);

  my $fmt_name = 'custom';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  if (open(my $fh, ">> $setup->{config_file}")) {
    if ($^O eq 'darwin') {
      # Mac OSX hack
      $sub_dir = '/private' . $sub_dir;
    }

    print $fh <<EOC;
<IfModule mod_amqp.c>
  AMQPEngine on
  AMQPServer $amqp_server:5672
  AMQPLog $setup->{log_file}
  LogFormat $fmt_name "%a %u"

  <Directory $sub_dir>
    AMQPLogOnEvent PWD $fmt_name
  </Directory>
</IfModule>
EOC
    unless (close($fh)) {
      die("Can't write $setup->{config_file}: $!");
    }

  } else {
    die("Can't open $setup->{config_file}: $!");
  }

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});
      $client->pwd();
      $client->cwd('test.d');
      $client->pwd();
      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 1, "Expected 1 record, got $nrecords");

    require JSON;
    my $json = $data->[0]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_log_on_event_per_dir_none {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $sub_dir = File::Spec->rel2abs("$tmpdir/test.d");
  mkpath($sub_dir);

  # Make sure that, if we're running as root, that the sub directory has
  # permissions/privs set for the account we create
  if ($< == 0) {
    unless (chmod(0755, $sub_dir)) {
      die("Can't set perms on $sub_dir to 0755: $!");
    }

    unless (chown($setup->{uid}, $setup->{gid}, $sub_dir)) {
      die("Can't set owner of $sub_dir to $setup->{uid}/$setup->{gid}: $!");
    }
  }

  my $fmt_name = 'custom';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  if (open(my $fh, ">> $setup->{config_file}")) {
    if ($^O eq 'darwin') {
      # Mac OSX hack
      $sub_dir = '/private' . $sub_dir;
    }

    print $fh <<EOC;
<IfModule mod_amqp.c>
  AMQPEngine on
  AMQPServer $amqp_server:5672
  AMQPLog $setup->{log_file}
  LogFormat $fmt_name "%a %u"

  <Directory $sub_dir>
    AMQPLogOnEvent PWD $fmt_name
  </Directory>

  <Directory $setup->{home_dir}>
    AMQPLogOnEvent none
  </Directory>
</IfModule>
EOC
    unless (close($fh)) {
      die("Can't write $setup->{config_file}: $!");
    }

  } else {
    die("Can't open $setup->{config_file}: $!");
  }

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      # Allow for server startup
      sleep(1);

      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});
      $client->pwd();
      $client->cwd('test.d');
      $client->pwd();
      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);
    if ($ENV{TEST_VERBOSE}) {
      use Data::Dumper;
      print STDERR "# ", Dumper($data), "\n";
    }

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 1, "Expected 1 record, got $nrecords");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_log_on_event_using_tls {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();
  my $ssl_verify = 'false';
  my $ssl_ca = File::Spec->rel2abs('t/etc/modules/mod_amqp/cacerts.pem');

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5671 \"\" \"\" \"\" ssl-ca:$ssl_ca ssl-verify:$ssl_verify",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      sleep(2);

      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 4 || $nrecords == 5,
      "Expected 4-5 records, got $nrecords");

    # Assert the message was published with the expected/configured properties
    my $content_type = $data->[0]->{props}->{content_type};
    $self->assert($content_type eq 'application/json',
      "Expected content type property 'application/json', got '$content_type'");

    my $type = $data->[0]->{props}->{type};
    $self->assert($type eq 'log',
      "Expected type property 'log', got '$type'");

    my $app_id = $data->[0]->{props}->{app_id};
    $self->assert($app_id eq 'proftpd',
      "Expected app ID property 'proftpd', got '$app_id'");

    my $ts = $data->[0]->{props}->{timestamp};
    $self->assert($ts > 0, "Expected timestamp property, got $ts");

    my $delivery_mode = $data->[0]->{props}->{delivery_mode};
    $self->assert($delivery_mode == 1,
      "Expected delivery mode property 1, got $delivery_mode");

    require JSON;
    my $json = $data->[3]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_log_on_event_using_tls_verify {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();
  my $ssl_verify = 'true';
  my $ssl_ca = File::Spec->rel2abs('t/etc/modules/mod_amqp/cacerts.pem');

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5671 \"\" \"\" \"\" ssl-ca:$ssl_ca ssl-verify:$ssl_verify",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      sleep(2);

      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    # Note that we expect ZERO records here.  The verification of the
    # RabbitMQ server will fail, either by hostname verification or by cert.
    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 0, "Expected 0 records, got $nrecords");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_log_on_event_using_tls_verify_peer {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();
  my $ssl_verify_hostname = 'false';
  my $ssl_verify_peer = 'true';
  my $ssl_ca = File::Spec->rel2abs('t/etc/modules/mod_amqp/cacerts.pem');

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5671 \"\" \"\" \"\" ssl-ca:$ssl_ca ssl-verify-hostname:$ssl_verify_hostname ssl-verify-peer:$ssl_verify_peer",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      sleep(2);

      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 4 || $nrecords == 5,
      "Expected 4-5 records, got $nrecords");

    # Assert the message was published with the expected/configured properties
    my $content_type = $data->[0]->{props}->{content_type};
    $self->assert($content_type eq 'application/json',
      "Expected content type property 'application/json', got '$content_type'");

    my $type = $data->[0]->{props}->{type};
    $self->assert($type eq 'log',
      "Expected type property 'log', got '$type'");

    my $app_id = $data->[0]->{props}->{app_id};
    $self->assert($app_id eq 'proftpd',
      "Expected app ID property 'proftpd', got '$app_id'");

    my $ts = $data->[0]->{props}->{timestamp};
    $self->assert($ts > 0, "Expected timestamp property, got $ts");

    my $delivery_mode = $data->[0]->{props}->{delivery_mode};
    $self->assert($delivery_mode == 1,
      "Expected delivery mode property 1, got $delivery_mode");

    require JSON;
    my $json = $data->[3]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_log_on_event_using_tls_client_cert {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();
  my $ssl_verify = 'false';
  my $ssl_cert = File::Spec->rel2abs('t/etc/modules/mod_amqp/client-cert.pem');
  my $ssl_key = File::Spec->rel2abs('t/etc/modules/mod_amqp/client-cert.pem');
  my $ssl_ca = File::Spec->rel2abs('t/etc/modules/mod_amqp/cacerts.pem');

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5671 \"\" \"\" \"\" ssl-ca:$ssl_ca ssl-verify:$ssl_verify ssl-cert:$ssl_cert ssl-key:$ssl_key",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      sleep(2);

      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 4 || $nrecords == 5,
      "Expected 4-5 records, got $nrecords");

    # Assert the message was published with the expected/configured properties
    my $content_type = $data->[0]->{props}->{content_type};
    $self->assert($content_type eq 'application/json',
      "Expected content type property 'application/json', got '$content_type'");

    my $type = $data->[0]->{props}->{type};
    $self->assert($type eq 'log',
      "Expected type property 'log', got '$type'");

    my $app_id = $data->[0]->{props}->{app_id};
    $self->assert($app_id eq 'proftpd',
      "Expected app ID property 'proftpd', got '$app_id'");

    my $ts = $data->[0]->{props}->{timestamp};
    $self->assert($ts > 0, "Expected timestamp property, got $ts");

    my $delivery_mode = $data->[0]->{props}->{delivery_mode};
    $self->assert($delivery_mode == 1,
      "Expected delivery mode property 1, got $delivery_mode");

    require JSON;
    my $json = $data->[3]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_opt_persistent_delivery {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5672",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name",
        "AMQPOptions PersistentDelivery",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 4 || $nrecords == 5,
      "Expected 4-5 records, got $nrecords");

    # Assert the message was published with the expected/configured properties
    my $content_type = $data->[0]->{props}->{content_type};
    $self->assert($content_type eq 'application/json',
      "Expected content type property 'application/json', got '$content_type'");

    my $type = $data->[0]->{props}->{type};
    $self->assert($type eq 'log',
      "Expected type property 'log', got '$type'");

    my $app_id = $data->[0]->{props}->{app_id};
    $self->assert($app_id eq 'proftpd',
      "Expected app ID property 'proftpd', got '$app_id'");

    my $ts = $data->[0]->{props}->{timestamp};
    $self->assert($ts > 0, "Expected timestamp property, got $ts");

    my $delivery_mode = $data->[0]->{props}->{delivery_mode};
    $self->assert($delivery_mode == 2,
      "Expected delivery mode property 2, got $delivery_mode");

    require JSON;
    my $json = $data->[3]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_config_app_id {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();
  my $app_id = "custom";

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5672",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name",
        "AMQPApplicationID $app_id",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 4 || $nrecords == 5,
      "Expected 4-5 records, got $nrecords");

    # Assert the message was published with the expected/configured properties
    my $content_type = $data->[0]->{props}->{content_type};
    $self->assert($content_type eq 'application/json',
      "Expected content type property 'application/json', got '$content_type'");

    my $type = $data->[0]->{props}->{type};
    $self->assert($type eq 'log',
      "Expected type property 'log', got '$type'");

    my $msg_app_id = $data->[0]->{props}->{app_id};
    $self->assert($msg_app_id eq $app_id,
      "Expected app ID property '$app_id', got '$msg_app_id'");

    my $ts = $data->[0]->{props}->{timestamp};
    $self->assert($ts > 0, "Expected timestamp property, got $ts");

    my $delivery_mode = $data->[0]->{props}->{delivery_mode};
    $self->assert($delivery_mode == 1,
      "Expected delivery mode property 1, got $delivery_mode");

    require JSON;
    my $json = $data->[3]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_config_timeout {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $amqp_server = get_rmq_host();

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        'AMQPTimeout 1',
        "AMQPServer $amqp_server:5673",
        "AMQPLog $setup->{log_file}",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    if (open(my $fh, "< $setup->{log_file}")) {
      my $saw_timeout_error = 0;

      while (my $line = <$fh>) {
        chomp($line);

        if ($ENV{TEST_VERBOSE}) {
          print STDERR "# $line\n";
        }

        if ($line =~ /operation now in progress/i) {
          $saw_timeout_error = 1;
          last;
        }
      }

      close($fh);

      $self->assert($saw_timeout_error,
        test_msg("Did not see expected TraceLog message"));

    } else {
      die("Can't read $setup->{log_file}: $!");
    }
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_config_msg_type {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();
  my $msg_type = "custom";

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5672",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name",
        "AMQPMessageType $msg_type",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 4 || $nrecords == 5,
      "Expected 4-5 records, got $nrecords");

    # Assert the message was published with the expected/configured properties
    my $content_type = $data->[0]->{props}->{content_type};
    $self->assert($content_type eq 'application/json',
      "Expected content type property 'application/json', got '$content_type'");

    my $type = $data->[0]->{props}->{type};
    $self->assert($type eq $msg_type,
      "Expected type property '$msg_type', got '$type'");

    my $msg_app_id = $data->[0]->{props}->{app_id};
    $self->assert($msg_app_id eq 'proftpd',
      "Expected app ID property 'proftpd', got '$msg_app_id'");

    my $ts = $data->[0]->{props}->{timestamp};
    $self->assert($ts > 0, "Expected timestamp property, got $ts");

    my $delivery_mode = $data->[0]->{props}->{delivery_mode};
    $self->assert($delivery_mode == 1,
      "Expected delivery mode property 1, got $delivery_mode");

    require JSON;
    my $json = $data->[3]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_config_msg_expires_before_expiry {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();

  # In ms
  my $msg_expires = "30000";

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5672",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name",
        "AMQPMessageExpires $msg_expires",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 4 || $nrecords == 5,
      "Expected 4-5 records, got $nrecords");

    # Assert the message was published with the expected/configured properties
    my $content_type = $data->[0]->{props}->{content_type};
    $self->assert($content_type eq 'application/json',
      "Expected content type property 'application/json', got '$content_type'");

    my $type = $data->[0]->{props}->{type};
    $self->assert($type eq 'log',
      "Expected type property 'log', got '$type'");

    my $msg_app_id = $data->[0]->{props}->{app_id};
    $self->assert($msg_app_id eq 'proftpd',
      "Expected app ID property 'proftpd', got '$msg_app_id'");

    my $ts = $data->[0]->{props}->{timestamp};
    $self->assert($ts > 0, "Expected timestamp property, got $ts");

    my $delivery_mode = $data->[0]->{props}->{delivery_mode};
    $self->assert($delivery_mode == 1,
      "Expected delivery mode property 1, got $delivery_mode");

    my $msg_expiry = $data->[0]->{props}->{expiration};
    $self->assert($msg_expiry eq $msg_expires,
      "Expected expiration property '$msg_expires', got '$msg_expiry'");

    require JSON;
    my $json = $data->[3]->{body};
    my $record = decode_json($json);

    my $expected = $setup->{user};
    $self->assert($record->{user} eq $expected,
      "Expected user '$expected', got '$record->{user}'");

    $expected = '127.0.0.1';
    $self->assert($record->{remote_ip} eq $expected,
      "Expected remote IP '$expected', got '$record->{remote_ip}'");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

sub amqp_config_msg_expires_after_expiry {
  my $self = shift;
  my $tmpdir = $self->{tmpdir};
  my $setup = test_setup($tmpdir, 'amqp');

  my $fmt_name = 'mod_amqp';
  my $queue = $fmt_name;
  rmq_queue_delete($queue);
  rmq_queue_declare($queue);

  my $amqp_server = get_rmq_host();

  # In ms
  my $msg_expires = "1";

  my $config = {
    PidFile => $setup->{pid_file},
    ScoreboardFile => $setup->{scoreboard_file},
    SystemLog => $setup->{log_file},
    TraceLog => $setup->{log_file},
    Trace => 'amqp:20 jot:20',

    AuthUserFile => $setup->{auth_user_file},
    AuthGroupFile => $setup->{auth_group_file},
    AuthOrder => 'mod_auth_file.c',
    UseIPv6 => 'off',

    IfModules => {
      'mod_delay.c' => {
        DelayEngine => 'off',
      },

      # Note: we need to use arrays here, since order of directives matters.
      'mod_amqp.c' => [
        'AMQPEngine on',
        "AMQPServer $amqp_server:5672",
        "AMQPLog $setup->{log_file}",
        "LogFormat $fmt_name \"%A %a %b %c %D %d %E %{epoch} %F %f %{gid} %g %H %h %I %{iso8601} %J %L %l %m %O %P %p %{protocol} %R %r %{remote-port} %S %s %T %t %U %u %{uid} %V %v %{version}\"",
        "AMQPLogOnEvent ALL $fmt_name",
        "AMQPMessageExpires $msg_expires",
      ],
    },
  };

  my ($port, $config_user, $config_group) = config_write($setup->{config_file},
    $config);

  # Open pipes, for use between the parent and child processes.  Specifically,
  # the child will indicate when it's done with its test by writing a message
  # to the parent.
  my ($rfh, $wfh);
  unless (pipe($rfh, $wfh)) {
    die("Can't open pipe: $!");
  }

  my $ex;

  # Fork child
  $self->handle_sigchld();
  defined(my $pid = fork()) or die("Can't fork: $!");
  if ($pid) {
    eval {
      my $client = ProFTPD::TestSuite::FTP->new('127.0.0.1', $port);
      $client->login($setup->{user}, $setup->{passwd});

      my $resp_code = $client->response_code();
      my $resp_msg = $client->response_msg(0);

      my $expected = 230;
      $self->assert($expected == $resp_code,
        "Expected response code $expected, got $resp_code");

      $expected = "User $setup->{user} logged in";
      $self->assert($expected eq $resp_msg,
        "Expected response message '$expected', got '$resp_msg'");

      $client->quit();
    };
    if ($@) {
      $ex = $@;
    }

    $wfh->print("done\n");
    $wfh->flush();

  } else {
    eval { server_wait($setup->{config_file}, $rfh) };
    if ($@) {
      warn($@);
      exit 1;
    }

    exit 0;
  }

  # Stop server
  server_stop($setup->{pid_file});
  $self->assert_child_ok($pid);

  eval {
    # Ensure that our messages have expired
    sleep(2);

    my $data = rmq_queue_getall($queue);

    my $nrecords = scalar(@$data);
    $self->assert($nrecords == 0,
      "Expected 0 records, got $nrecords");
  };
  if ($@) {
    $ex = $@;
  }

  test_cleanup($setup->{log_file}, $ex);
}

1;

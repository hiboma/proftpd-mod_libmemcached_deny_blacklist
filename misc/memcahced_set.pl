#!/usr/bin/perl

use strict;
use warnings;
use Cache::Memcached;

my $memd = new Cache::Memcached { 'servers' => [ "127.0.0.1:11211"]  };
my $key  = shift || die "[usage] $0 <key>";
$memd->set($key, 1);
printf "%s => '%s'\n", $key, $memd->get($key);

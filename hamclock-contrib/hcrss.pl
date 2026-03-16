#!/usr/bin/perl
# read an RSS feed and send the titles to hamclock.

# helpers
use strict;
use warnings;
use Text::Unidecode;
use HTML::Entities;
use URI::Escape;
use XML::LibXML;
use LWP::UserAgent;

my $HC_MAX = 15;                # max n titles HamClock can use

# require hamclock ip and rss feed
&usage() unless (@ARGV == 2);
my ($hc_ip, $rss_url) = @ARGV;

# retrieve url
my $rss_ua = LWP::UserAgent->new;
$rss_ua->ssl_opts(verify_hostname => 0);                # disable SSL for now
my $rss_response = $rss_ua->get($rss_url);
$rss_response->is_success or die "$rss_url: " . $rss_response->status_line . "\n";

# crack as xml
my $rss_dom = eval { XML::LibXML->load_xml(string => $rss_response->content) };
defined($rss_dom) or die "$rss_url: $@\n";

# collect titles as a multi-line string
my $titles;
my $n_titles = 0;
foreach my $t ($rss_dom->findnodes('/rss/channel/item/title')) {
    my $title = $t->to_literal();
    $title = uri_unescape($title);                      # translate hex codes
    $title = unidecode(decode_entities($title));        # no html entities
    $titles .= $title . "\n";
    $n_titles++;
}
if ($n_titles > $HC_MAX) {
    print "Found $n_titles rss titles -- 15 will fit in HamClock\n";
} else {
    print "Found $n_titles rss titles\n";
}

# prepare hamclock request
my $hc_url = "http://$hc_ip:8080/set_rss?file";
my $hc_req = HTTP::Request->new(POST => $hc_url);
$hc_req->content($titles);

# send and done
my $hc_ua = LWP::UserAgent->new;
my $hc_resp = $hc_ua->request($hc_req);
if ($hc_resp->is_success) {
    print $hc_resp->decoded_content . "\n";
    exit 0;
} else {
    printf "HamClock response: %s\n", $hc_resp->content;
    exit 1;
}


##################################################################################
#
#  supporting subroutines
#
##################################################################################

# report usage and exit
sub usage
{
    $0 =~ s:.*/::;
    print STDERR "Purpose: read an RSS feed and send the titles to hamclock.\n";
    print STDERR "Usage: $0 hamclock_ip feed_url\n";
    print STDERR "Example: $0 192.168.7.117 https://www.arrl.org/news/rss\n";
    exit(1);
}

<?php
/*
    $Id$
    
    Copyright (C) 2007 Marcel Wiget <mwiget@mac.com>.
    All rights reserved.
    
    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    
    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
    
    THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
    AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
    AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
    OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.

*/
    
/* include all configuration functions */
require_once("functions.inc");

$lockfile = "{$g['varrun_path']}/voucher.lock";
$dirtyfile = "{$g['varrun_path']}/voucher.dirty";

/* 
 *Authenticate a voucher and return the remaining time credit in minutes
 * if $test is set, don't mark the voucher as used nor add it to the list
 * of active vouchers
 */
function voucher_auth($voucher_received, $test = 0) {

    global $g, $config, $dirtyfile;

    // if $test is set, simply test the voucher. Don't change anything
    // but return a more verbose error and result message back

    if (! $test)
        voucher_lock();

    // read rolls into assoc array with rollid as key and minutes as value
    $a_roll = &$config['voucher']['roll'];
    foreach ($a_roll as $rollent) {
        $tickets_per_roll[$rollent['number']] = $rollent['count'];
        $minutes_per_roll[$rollent['number']] = $rollent['minutes'];
    }

    // split into an array. Useful for multiple vouchers given
    $a_vouchers_received = split("[\t\n\r ]+",$voucher_received); 
    $error = 0;
    $test_result = array();     // used to display for voucher test option in GUI
    $total_minutes = 0;
    $first_voucher = "";
    $first_voucher_roll = 0;

    // go through all received vouchers, check their valid and extract
    // Roll# and Ticket# using the external readvoucher binary

    foreach ($a_vouchers_received as $voucher) {

        $v = escapeshellarg($voucher);
        if (strlen($voucher) < 3)
            continue;   // seems too short to be a voucher!

        $result = exec("/usr/local/bin/voucher -c {$g['varetc_path']}/voucher.cfg -k {$g['varetc_path']}/voucher.public -- $v");
        list($status, $roll, $nr) = explode(" ", $result);
        if ($status == "OK") {
            if (!$first_voucher) 
            {
                $first_voucher = $voucher;  // store first voucher. Thats the one we give the timecredit
                $first_voucher_roll = $roll;
            }
            // check if we have this ticket on a registered roll for this ticket 
            if ($tickets_per_roll[$roll] && ($nr <= $tickets_per_roll[$roll])) {
                // voucher is from a registered roll. 
                if (!isset($active_vouchers[$roll]))
                    $active_vouchers[$roll] = voucher_read_active_db($roll);
                // valid voucher. Store roll# and ticket#
                if ($line = $active_vouchers[$roll][$voucher]) {
                    list($timestamp,$minutes) = explode(",", $line);
                    // we have an already active voucher here.
                    $remaining = intval((($timestamp + 60*$minutes) - time())/60);
                    $test_result[] = "$voucher ($roll/$nr) active and good for $remaining Minutes";
                    $total_minutes += $remaining;
                } else {
                    // voucher not used. Check if ticket Id is on the roll (not too high)
                    // and if the ticket is marked used.
                    // check if voucher already marked as used
                    if (!isset($bitstring[$roll]))
                        $bitstring[$roll] = voucher_read_used_db($roll);
                    $pos = $nr >> 3; // divide by 8 -> octet
                    $mask = 1 << ($nr % 8);
                    if (ord($bitstring[$roll][$pos]) & $mask) {
                        $test_result[] = "$voucher ($roll/$nr) already used and expired";
                        $total_minutes = -1;    // voucher expired
                        $error++;
                    } else {
                        // mark bit for this voucher as used
                        $bitstring[$roll][$pos] = chr(ord($bitstring[$roll][$pos]) | $mask);
                        $test_result[] = "$voucher ($roll/$nr) good for {$minutes_per_roll[$roll]} Minutes";
                        $total_minutes += $minutes_per_roll[$roll];
                    }
                }
            } else {
                $test_result[] = "$voucher ($roll/$nr): not found on any registererd Roll";
            }
        } else {
            // hmm, thats weired ... not what I expected
            $test_result[] = "$voucher invalid: $result !!";
            $error++;
        }
    }

    // if this was a test call, we're done. Return the result.
    if ($test) {
        if ($error) {
            $test_result[] = "Access denied!";
        } else {
            $test_result[] = "Access granted for $total_minutes Minutes in total.";
        }
        return $test_result;
    }

    // if we had an error (one of the vouchers is invalid), return 0.
    // Discussion: we could return the time remaining for good vouchers, but then
    // the user wouldn't know that he used at least one invalid voucher.

    if ($error) {
        voucher_unlock();
        if ($total_minutes > 0)     // probably not needed, but want to make sure
            $total_minutes = 0;     // we only report -1 (expired) or 0 (no access)
        return $total_minutes;       // well, at least one voucher had errors. Say NO ACCESS
    }

    // All given vouchers were valid and this isn't simply a test.
    // Write back the used DB's

    if (is_array($bitstring))
        foreach ($bitstring as $roll => $used)
            voucher_write_used_db($roll, base64_encode($used));

    // Active DB: we only add the first voucher if multiple given
    // and give that one all the time credit. This allows the user to logout and
    // log in later using just the first voucher. It also keeps username limited
    // to one voucher and that voucher shows the correct time credit in 'active vouchers'

    if ($line = $active_vouchers[$first_voucher_roll][$first_voucher]) {
        list($timestamp, $minutes) = explode(",", $line);
    } else {
        $timestamp = time();    // new voucher
        $minutes = $total_minutes;
    }

    $active_vouchers[$first_voucher_roll][$first_voucher] = "$timestamp,$minutes";
    voucher_write_active_db($roll, $active_vouchers[$first_voucher_roll]);

    // mark the DB's as dirty.
    if ($fd = fopen($dirtyfile, "w"))
        fclose($fd);

    voucher_unlock();

    return $total_minutes;
}

function voucher_configure() {
    global $config, $g;
    
    /* kill any running minicron */
    killbypid("{$g['varrun_path']}/vouchercron.pid");

    if (isset($config['voucher']['enable'])) {

        if ($g['booting']) {
            echo "Enabling voucher support... ";
        }

        // start cron if we're asked to save runtime DB periodically
        // to XML config if it changed
        $croninterval = $config['voucher']['saveinterval'] * 60; // need seconds. Config has minutes
        if ($croninterval) {
            /* start pruning process (interval defaults to 60 seconds) */
            mwexec("/usr/local/bin/minicron $croninterval {$g['varrun_path']}/vouchercron.pid " .
                    "/etc/rc.savevoucher");
        }

        /* write public key used to verify vouchers */
        $pubkey = base64_decode($config['voucher']['publickey']);
        $fd = fopen("{$g['varetc_path']}/voucher.public", "w");
        if (!$fd) {
            printf("Error: cannot write voucher.public\n");
            return 1;
        }
        chmod("{$g['varetc_path']}/voucher.public", 0600);
        fwrite($fd, $pubkey);
        fclose($fd);

        /* write config file used by voucher binary to decode vouchers */
        $fd = fopen("{$g['varetc_path']}/voucher.cfg", "w");
        if (!$fd) {
            printf("Error: cannot write voucher.cfg\n");
            return 1;
        }
        chmod("{$g['varetc_path']}/voucher.cfg", 0600);
        fwrite($fd, "{$config['voucher']['rollbits']},{$config['voucher']['ticketbits']},{$config['voucher']['checksumbits']},{$config['voucher']['magic']},{$config['voucher']['charset']}\n");
        fclose($fd);

        if ($g['booting']) {

            // create active and used DB per roll on ramdisk from config
            $a_roll = &$config['voucher']['roll'];
            voucher_lock();

            foreach ($a_roll as $rollent) {

                $roll = $rollent['number'];
                voucher_write_used_db($roll, $rollent['used']);
                $minutes = $rollent['minutes'];
                $active_vouchers = array();
                $a_active = &$rollent['active'];
                if (is_array($a_active)) {
                    foreach ($a_active as $activent) {
                        $voucher = $activent['voucher'];
                        $timestamp = $activent['timestamp'];
                        $minutes = $activent['minutes'];
                        // its tempting to check for expired timestamps, but during
                        // bootup, we most likely don't have the correct time time.
                        $active_vouchers[$voucher] = "$timestamp,$minutes";
                    }
                }
                voucher_write_active_db($roll, $active_vouchers);
            }

            voucher_unlock();
            echo "done\n";
        }
    }
    return 0;
}

/* lock captive portal information, decide that the lock file is stale after
   10 seconds */
function voucher_lock() {

    global $lockfile;

    $n = 0;
    while ($n < 10) {
        /* open the lock file in append mode to avoid race condition */
        if ($fd = fopen($lockfile, "x")) {
            /* succeeded */
            fclose($fd);
            return;
        } else {
            /* file locked, wait and try again */
            sleep(1);
            $n++;
        }
    }
}
/* unlock captive portal information file */
function voucher_unlock() {

    global $lockfile;

    if (file_exists($lockfile))
        unlink($lockfile);
}

/* write bitstring of used vouchers to ramdisk. 
 * Bitstring must already be base64_encoded!
 */
function voucher_write_used_db($roll, $vdb) {

    global $g;

    $fd = fopen("{$g['vardb_path']}/voucher_used_$roll.db", "w");
    if ($fd) {
        fwrite($fd, $vdb . "\n");
        fclose($fd);
    } else {
        voucher_log(LOG_ERR, "cant write {$g['vardb_path']}/voucher_used_$roll.db");
    }
}

/* return assoc array of active vouchers with activation timestamp
 * voucher is index. 
 */
function voucher_read_active_db($roll) {

    global $g;

    $active = array();
    $dirty = 0;
    $file = "{$g['vardb_path']}/voucher_active_$roll.db";
    if (file_exists($file)) {
        $fd = fopen($file, "r");
        if ($fd) {
            while (!feof($fd)) {
                $line = trim(fgets($fd));
                if ($line) {
                    list($voucher,$timestamp,$minutes) = explode(",", $line); // voucher,timestamp
                    if ((($timestamp + 60*$minutes) - time()) > 0) {
                        $active[$voucher] = "$timestamp,$minutes";
                    } else {
                        $dirty=1;
                    }
                }
            }
            fclose($fd);
            if ($dirty) // if we found expired entries, lets save our snapshot
                voucher_write_active_db($roll, $active);
        }
    }
    return $active;
}

/* store array of active vouchers back to DB */
function voucher_write_active_db($roll, $active) {

    global $g;

    $fd = fopen("{$g['vardb_path']}/voucher_active_$roll.db", "w");
    if ($fd) {
        foreach($active as $voucher => $value)
            fwrite($fd, "$voucher,$value\n");
        fclose($fd);
    }
}

/* return how many vouchers are marked used on a roll */
function voucher_used_count($roll) {

    global $g;

    $bitstring = voucher_read_used_db($roll);
    $max = strlen($bitstring) * 8;
    $used = 0;
    for ($i = 1; $i <= $max; $i++) {
        // check if ticket already used or not. 
        $pos = $i >> 3;            // divide by 8 -> octet
        $mask = 1 << (($i % 8)-1);  // mask to test bit in octet
        if (ord($bitstring[$pos]) & $mask)
            $used++;
    }   
    return $used;
}

function voucher_read_used_db($roll) {

    global $g;

    $vdb = "";
    $file = "{$g['vardb_path']}/voucher_used_$roll.db";
    if (file_exists($file)) {
        $fd = fopen($file, "r");
        if ($fd) {
            $vdb = trim(fgets($fd));
            fclose($fd);
        } else {
            voucher_log(LOG_ERR, "cant read {$g['vardb_path']}/voucher_used_$roll.db");
        }
    }
    return base64_decode($vdb);
}

function voucher_unlink_db($roll) {

    global $g;
    unlink("{$g['vardb_path']}/voucher_used_$roll.db");
    unlink("{$g['vardb_path']}/voucher_active_$roll.db");
}

/* we share the log with captiveportal for now */
function voucher_log($priority, $message) {

    define_syslog_variables();
    $message = trim($message);
    openlog("logportalauth", LOG_PID, LOG_LOCAL4);
    syslog($priority, "Voucher: " . $message);
    closelog();
}

/* Save active and used voucher DB into XML config and write it to flash
 * Called during reboot -> system_reboot_cleanup() and minicron
 */
function voucher_save_db_to_config() {

    global $config, $g, $dirtyfile;
    
    if (!isset($config['voucher']['enable']) || $config['voucher']['saveinterval'] == 0) 
        return;   // no vouchers or don't want to save DB's

    if (!file_exists($dirtyfile))
        return;     // nothing changed.

    voucher_lock(); 

    // walk all active rolls and save runtime DB's to flash
    $a_roll = &$config['voucher']['roll'];
//    foreach ($a_roll as $rollent) {
    while (list($key, $value) = each($a_roll)) {
        $rollent = &$a_roll[$key];
        $roll = $rollent['number'];
        $bitmask = voucher_read_used_db($roll);
        $rollent['used'] = base64_encode($bitmask);
        $active_vouchers = voucher_read_active_db($roll);
        $db = array();
        foreach($active_vouchers as $voucher => $line) {
            list($timestamp,$minutes) = explode(",", $line);
            $activent['voucher'] = $voucher;
            $activent['timestamp'] = $timestamp;
            $activent['minutes'] = $minutes;
            $db[] = $activent;
        }
        $rollent['active'] = $db;
    }
    unlink($dirtyfile);
    write_config();
    voucher_unlock(); 
    return;
}

?>

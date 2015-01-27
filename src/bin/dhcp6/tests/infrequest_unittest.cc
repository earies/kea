// Copyright (C) 2015  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <config.h>
#include <dhcp/tests/iface_mgr_test_config.h>
#include <dhcp6/tests/dhcp6_test_utils.h>
#include <dhcp6/tests/dhcp6_client.h>
#include <dhcp/option6_addrlst.h>
#include <dhcp/option6_client_fqdn.h>

using namespace isc;
using namespace isc::dhcp;
using namespace isc::dhcp::test;
using namespace isc::test;

namespace {

/// @brief Set of JSON configurations used by the Information-Request unit tests.
///
/// - Configuration 0:
///   - one subnet used on eth0 interface
///     - with address and prefix pools
///     - dns-servers option
/// - Configuation 1:
///   - one subnet used on eth0 interface
///     - no addresses or prefixes
///     - domain-search option
/// - Configuration 2:
///   - one subnet used on eth0 interface
///     - dns-servers option for subnet
///   - sip-servers defined in global scope
/// - Configuration 3:
///   - nis-server, nis-domain specified in global scope
///   - no subnets defined
const char* CONFIGS[] = {
    // Configuration 0
    "{ \"interfaces\": [ \"*\" ],"
        "\"preferred-lifetime\": 3000,"
        "\"rebind-timer\": 2000, "
        "\"renew-timer\": 1000, "
        "\"subnet6\": [ { "
        "    \"pools\": [ { \"pool\": \"2001:db8:2::/64\" } ],"
        "    \"pd-pools\": ["
        "        { \"prefix\": \"2001:db8:3::\", "
        "          \"prefix-len\": 48, "
        "          \"delegated-len\": 64"
        "        } ],"
        "    \"option-data\": [ {"
        "        \"name\": \"dns-servers\","
        "        \"data\": \"2001:db8::1, 2001:db8::2\""
        "    } ],"
        "    \"subnet\": \"2001:db8::/32\", "
        "    \"interface\": \"eth0\""
        " } ],"
        "\"valid-lifetime\": 4000 }",
    // Configuration 1
    "{ \"interfaces\": [ \"*\" ],"
        "\"preferred-lifetime\": 3000,"
        "\"rebind-timer\": 2000, "
        "\"renew-timer\": 1000, "
        "\"subnet6\": [ { "
        "    \"option-data\": [ {"
        "    \"name\": \"sip-server-addr\","
        "    \"data\": \"2001:db8::abcd\""
        "    } ],"
        "    \"subnet\": \"2001:db8::/32\", "
        "    \"interface\": \"eth0\""
        " } ],"
        "\"valid-lifetime\": 4000 }",
    // Configuration 2
    "{ \"interfaces\": [ \"*\" ],"
        "\"preferred-lifetime\": 3000,"
        "\"rebind-timer\": 2000, "
        "\"renew-timer\": 1000, "
        "    \"option-data\": [ {"
        "        \"name\": \"sip-server-dns\","
        "        \"data\": \"2001:db8::1\""
        "    } ],"
        "\"subnet6\": [ { "
        "    \"subnet\": \"2001:db8::/32\", "
        "    \"interface\": \"eth0\","
        "    \"option-data\": [ {"
        "        \"name\": \"dns-servers\","
        "        \"data\": \"2001:db8::2\""
        "    } ]"
        " } ],"
        "\"valid-lifetime\": 4000 }",
    // Configuration 2
    "{ \"interfaces\": [ \"*\" ],"
        "\"option-data\": [ {"
        "    \"name\": \"nis-servers\","
        "    \"data\": \"2001:db8::1, 2001:db8::2\""
        " } ],"
        "\"subnet6\": [ ]"
        "}"
};

/// @brief Test fixture class for testing 4-way exchange: Solicit-Advertise,
/// Request-Reply.
class InfRequestTest : public Dhcpv6SrvTest {
public:
    /// @brief Constructor.
    ///
    /// Sets up fake interfaces.
    InfRequestTest()
        : Dhcpv6SrvTest(),
          iface_mgr_test_config_(true) {
    }

    /// @brief Interface Manager's fake configuration control.
    IfaceMgrTestConfig iface_mgr_test_config_;
};

/// Check that server processes correctly an incoming inf-request in a
/// typical subnet that has also address and prefix pools.
TEST_F(InfRequestTest, infRequestBasic) {
    Dhcp6Client client;

    // Configure client to request IA_PD.
    configure(CONFIGS[0], *client.getServer());
    // Make sure we ended-up having expected number of subnets configured.
    const Subnet6Collection* subnets = CfgMgr::instance().getCurrentCfg()->
        getCfgSubnets6()->getAll();
    ASSERT_EQ(1, subnets->size());

    // Perform 2-way exchange (Inf-request/reply)
    ASSERT_NO_THROW(client.doInfRequest());

    // Confirm that there's a response
    Pkt6Ptr response = client.getContext().response_;
    ASSERT_TRUE(response);

    Option6AddrLstPtr dns = boost::dynamic_pointer_cast<Option6AddrLst>
                            (response->getOption(D6O_NAME_SERVERS));
    ASSERT_TRUE(dns);
    Option6AddrLst::AddressContainer addrs = dns->getAddresses();
    ASSERT_EQ(2, addrs.size());
    EXPECT_EQ("2001:db8::1", addrs[0].toText());
    EXPECT_EQ("2001:db8::2", addrs[0].toText());
}

/// Check that server processes correctly an incoming inf-request
/// that does not hold client-id. It's so called anonymous inf-request.
/// Uncommon, but certainly valid behavior.
TEST_F(InfRequestTest, infRequestAnonymous) {
    Dhcp6Client client;

    // Configure client to request IA_PD.
    configure(CONFIGS[0], *client.getServer());
    // Make sure we ended-up having expected number of subnets configured.
    const Subnet6Collection* subnets = CfgMgr::instance().getCurrentCfg()->
        getCfgSubnets6()->getAll();
    ASSERT_EQ(1, subnets->size());

    // Perform 2-way exchange (Inf-request/reply)
    client.sendClientId(false);
    ASSERT_NO_THROW(client.doInfRequest());

    // Confirm that there's a response
    Pkt6Ptr response = client.getContext().response_;
    ASSERT_TRUE(response);

    Option6AddrLstPtr dns = boost::dynamic_pointer_cast<Option6AddrLst>
                            (response->getOption(D6O_NAME_SERVERS));
    ASSERT_TRUE(dns);
    Option6AddrLst::AddressContainer addrs = dns->getAddresses();
    ASSERT_EQ(2, addrs.size());
    EXPECT_EQ("2001:db8::1", addrs[0].toText());
    EXPECT_EQ("2001:db8::2", addrs[0].toText());
}

/// Check that server processes correctly an incoming inf-request
/// if there is a subnet without any addresses or prefixes configured.
TEST_F(InfRequestTest, infRequestStateless) {
    Dhcp6Client client;

    // Configure client to request IA_PD.
    configure(CONFIGS[1], *client.getServer());
    // Make sure we ended-up having expected number of subnets configured.
    const Subnet6Collection* subnets = CfgMgr::instance().getCurrentCfg()->
        getCfgSubnets6()->getAll();
    ASSERT_EQ(1, subnets->size());

    // Perform 2-way exchange (Inf-request/reply)
    ASSERT_NO_THROW(client.doInfRequest());

    // Confirm that there's a response
    Pkt6Ptr response = client.getContext().response_;
    ASSERT_TRUE(response);

    Option6AddrLstPtr sip = boost::dynamic_pointer_cast<Option6AddrLst>
                            (response->getOption(D6O_SIP_SERVERS_ADDR));
    ASSERT_TRUE(sip);
    Option6AddrLst::AddressContainer addrs = sip->getAddresses();
    ASSERT_EQ(1, addrs.size());
    EXPECT_EQ("2001:db8::abcd", addrs[0].toText());
}


} // end of anonymous namespace

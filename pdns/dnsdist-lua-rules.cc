/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "dnsdist.hh"
#include "dnsdist-lua.hh"
#include "dnsdist-rules.hh"

std::shared_ptr<DNSRule> makeRule(const luadnsrule_t& var)
{
  if (var.type() == typeid(std::shared_ptr<DNSRule>))
    return *boost::get<std::shared_ptr<DNSRule>>(&var);

  SuffixMatchNode smn;
  NetmaskGroup nmg;
  auto add=[&](string src) {
    try {
      nmg.addMask(src); // need to try mask first, all masks are domain names!
    } catch(...) {
      smn.add(DNSName(src));
    }
  };

  if (var.type() == typeid(string))
    add(*boost::get<string>(&var));

  else if (var.type() == typeid(LuaArray<std::string>))
    for(const auto& a : *boost::get<LuaArray<std::string>>(&var))
      add(a.second);

  else if (var.type() == typeid(DNSName))
    smn.add(*boost::get<DNSName>(&var));

  else if (var.type() == typeid(LuaArray<DNSName>))
    for(const auto& a : *boost::get<LuaArray<DNSName>>(&var))
      smn.add(a.second);

  if(nmg.empty())
    return std::make_shared<SuffixMatchNodeRule>(smn);
  else
    return std::make_shared<NetmaskGroupRule>(nmg, true);
}

static boost::uuids::uuid makeRuleID(std::string& id)
{
  if (id.empty()) {
    return getUniqueID();
  }

  return getUniqueID(id);
}

void parseRuleParams(boost::optional<luaruleparams_t> params, boost::uuids::uuid& uuid, std::string& name, uint64_t& creationOrder)
{
  static uint64_t s_creationOrder = 0;

  string uuidStr;

  if (params) {
    if (params->count("uuid")) {
      uuidStr = params->at("uuid");
    }
    if (params->count("name")) {
      name = params->at("name");
    }
  }

  uuid = makeRuleID(uuidStr);
  creationOrder = s_creationOrder++;
}

typedef LuaAssociativeTable<boost::variant<bool, int, std::string, LuaArray<int> > > ruleparams_t;

template<typename T>
static std::string rulesToString(const std::vector<T>& rules, boost::optional<ruleparams_t> vars)
{
  int num = 0;
  bool showUUIDs = false;
  size_t truncateRuleWidth = string::npos;
  std::string result;

  if (vars) {
    if (vars->count("showUUIDs")) {
      showUUIDs = boost::get<bool>((*vars)["showUUIDs"]);
    }
    if (vars->count("truncateRuleWidth")) {
      truncateRuleWidth = boost::get<int>((*vars)["truncateRuleWidth"]);
    }
  }

  if (showUUIDs) {
    boost::format fmt("%-3d %-30s %-38s %9d %9d %-56s %s\n");
    result += (fmt % "#" % "Name" % "UUID" % "Cr. Order" % "Matches" % "Rule" % "Action").str();
    for(const auto& lim : rules) {
      string desc = lim.d_rule->toString().substr(0, truncateRuleWidth);
      result += (fmt % num % lim.d_name % boost::uuids::to_string(lim.d_id) % lim.d_creationOrder % lim.d_rule->d_matches % desc % lim.d_action->toString()).str();
      ++num;
    }
  }
  else {
    boost::format fmt("%-3d %-30s %9d %-56s %s\n");
    result += (fmt % "#" % "Name" % "Matches" % "Rule" % "Action").str();
    for(const auto& lim : rules) {
      string desc = lim.d_rule->toString().substr(0, truncateRuleWidth);
      result += (fmt % num % lim.d_name %  lim.d_rule->d_matches % desc % lim.d_action->toString()).str();
      ++num;
    }
  }
  return result;
}

template<typename T>
static void showRules(GlobalStateHolder<vector<T> > *someRuleActions, boost::optional<ruleparams_t> vars) {
  setLuaNoSideEffect();

  auto rules = someRuleActions->getLocal();
  g_outputBuffer += rulesToString(*rules, vars);
}

template<typename T>
static void rmRule(GlobalStateHolder<vector<T> > *someRuleActions, boost::variant<unsigned int, std::string> id) {
  setLuaSideEffect();
  auto rules = someRuleActions->getCopy();
  if (auto str = boost::get<std::string>(&id)) {
    try {
      const auto uuid = getUniqueID(*str);
      if (rules.erase(std::remove_if(rules.begin(),
                                     rules.end(),
                                     [uuid](const T& a) { return a.d_id == uuid; }),
                      rules.end()) == rules.end()) {
        g_outputBuffer = "Error: no rule matched\n";
        return;
      }
    }
    catch (const std::runtime_error& e) {
      /* it was not an UUID, let's see if it was a name instead */
      if (rules.erase(std::remove_if(rules.begin(),
                                     rules.end(),
                                     [&str](const T& a) { return a.d_name == *str; }),
                      rules.end()) == rules.end()) {
        g_outputBuffer = "Error: no rule matched\n";
        return;
      }
    }
  }
  else if (auto pos = boost::get<unsigned int>(&id)) {
    if (*pos >= rules.size()) {
      g_outputBuffer = "Error: attempt to delete non-existing rule\n";
      return;
    }
    rules.erase(rules.begin()+*pos);
  }
  someRuleActions->setState(std::move(rules));
}

template<typename T>
static void moveRuleToTop(GlobalStateHolder<vector<T> > *someRuleActions) {
  setLuaSideEffect();
  auto rules = someRuleActions->getCopy();
  if(rules.empty())
    return;
  auto subject = *rules.rbegin();
  rules.erase(std::prev(rules.end()));
  rules.insert(rules.begin(), subject);
  someRuleActions->setState(std::move(rules));
}

template<typename T>
static void mvRule(GlobalStateHolder<vector<T> > *someRespRuleActions, unsigned int from, unsigned int to) {
  setLuaSideEffect();
  auto rules = someRespRuleActions->getCopy();
  if(from >= rules.size() || to > rules.size()) {
    g_outputBuffer = "Error: attempt to move rules from/to invalid index\n";
    return;
  }
  auto subject = rules[from];
  rules.erase(rules.begin()+from);
  if(to > rules.size())
    rules.push_back(subject);
  else {
    if(from < to)
      --to;
    rules.insert(rules.begin()+to, subject);
  }
  someRespRuleActions->setState(std::move(rules));
}

template<typename T>
static std::vector<T> getTopRules(const std::vector<T>& rules, unsigned int top)
{
  std::vector<std::pair<size_t, size_t>> counts;
  counts.reserve(rules.size());

  size_t pos = 0;
  for (const auto& rule : rules) {
    counts.push_back({rule.d_rule->d_matches.load(), pos});
    pos++;
  }

  sort(counts.begin(), counts.end(), [](const decltype(counts)::value_type& a,
                                        const decltype(counts)::value_type& b) {
    return b.first < a.first;
  });

  std::vector<T> results;
  results.reserve(top);

  size_t count = 0;
  for (const auto& entry : counts) {
    results.emplace_back(rules.at(entry.second));
    ++count;
    if (count == top) {
      break;
    }
  }

  return results;
}

void setupLuaRules(LuaContext& luaCtx)
{
  luaCtx.writeFunction("makeRule", makeRule);

  luaCtx.registerFunction<string(std::shared_ptr<DNSRule>::*)()const>("toString", [](const std::shared_ptr<DNSRule>& rule) { return rule->toString(); });

  luaCtx.writeFunction("showResponseRules", [](boost::optional<ruleparams_t> vars) {
      showRules(&g_respruleactions, vars);
    });

  luaCtx.writeFunction("rmResponseRule", [](boost::variant<unsigned int, std::string> id) {
      rmRule(&g_respruleactions, id);
    });

  luaCtx.writeFunction("mvResponseRuleToTop", []() {
      moveRuleToTop(&g_respruleactions);
    });

  luaCtx.writeFunction("mvResponseRule", [](unsigned int from, unsigned int to) {
      mvRule(&g_respruleactions, from, to);
    });

  luaCtx.writeFunction("showCacheHitResponseRules", [](boost::optional<ruleparams_t> vars) {
      showRules(&g_cachehitrespruleactions, vars);
    });

  luaCtx.writeFunction("rmCacheHitResponseRule", [](boost::variant<unsigned int, std::string> id) {
      rmRule(&g_cachehitrespruleactions, id);
    });

  luaCtx.writeFunction("mvCacheHitResponseRuleToTop", []() {
      moveRuleToTop(&g_cachehitrespruleactions);
    });

  luaCtx.writeFunction("mvCacheHitResponseRule", [](unsigned int from, unsigned int to) {
      mvRule(&g_cachehitrespruleactions, from, to);
    });

  luaCtx.writeFunction("showCacheInsertedResponseRules", [](boost::optional<ruleparams_t> vars) {
    showRules(&g_cacheInsertedRespRuleActions, vars);
  });

  luaCtx.writeFunction("rmCacheInsertedResponseRule", [](boost::variant<unsigned int, std::string> id) {
    rmRule(&g_cacheInsertedRespRuleActions, id);
  });

  luaCtx.writeFunction("mvCacheInsertedResponseRuleToTop", []() {
    moveRuleToTop(&g_cacheInsertedRespRuleActions);
  });

  luaCtx.writeFunction("mvCacheInsertedResponseRule", [](unsigned int from, unsigned int to) {
    mvRule(&g_cacheInsertedRespRuleActions, from, to);
  });

  luaCtx.writeFunction("showSelfAnsweredResponseRules", [](boost::optional<ruleparams_t> vars) {
      showRules(&g_selfansweredrespruleactions, vars);
    });

  luaCtx.writeFunction("rmSelfAnsweredResponseRule", [](boost::variant<unsigned int, std::string> id) {
      rmRule(&g_selfansweredrespruleactions, id);
    });

  luaCtx.writeFunction("mvSelfAnsweredResponseRuleToTop", []() {
      moveRuleToTop(&g_selfansweredrespruleactions);
    });

  luaCtx.writeFunction("mvSelfAnsweredResponseRule", [](unsigned int from, unsigned int to) {
      mvRule(&g_selfansweredrespruleactions, from, to);
    });

  luaCtx.writeFunction("rmRule", [](boost::variant<unsigned int, std::string> id) {
      rmRule(&g_ruleactions, id);
    });

  luaCtx.writeFunction("mvRuleToTop", []() {
      moveRuleToTop(&g_ruleactions);
    });

  luaCtx.writeFunction("mvRule", [](unsigned int from, unsigned int to) {
      mvRule(&g_ruleactions, from, to);
    });

  luaCtx.writeFunction("clearRules", []() {
      setLuaSideEffect();
      g_ruleactions.modify([](decltype(g_ruleactions)::value_type& ruleactions) {
          ruleactions.clear();
        });
    });

  luaCtx.writeFunction("setRules", [](const LuaArray<std::shared_ptr<DNSDistRuleAction>>& newruleactions) {
      setLuaSideEffect();
      g_ruleactions.modify([newruleactions](decltype(g_ruleactions)::value_type& gruleactions) {
          gruleactions.clear();
          for (const auto& pair : newruleactions) {
            const auto& newruleaction = pair.second;
            if (newruleaction->d_action) {
              auto rule = makeRule(newruleaction->d_rule);
              gruleactions.push_back({std::move(rule), newruleaction->d_action, newruleaction->d_name, newruleaction->d_id, newruleaction->d_creationOrder});
            }
          }
        });
    });

  luaCtx.writeFunction("getTopRules", [](boost::optional<unsigned int> top) {
    setLuaNoSideEffect();
    auto rules = g_ruleactions.getLocal();
    return getTopRules(*rules, top.get_value_or(10));
  });

  luaCtx.writeFunction("topRules", [](boost::optional<unsigned int> top, boost::optional<ruleparams_t> vars) {
    setLuaNoSideEffect();
    auto rules = g_ruleactions.getLocal();
    return rulesToString(getTopRules(*rules, top.get_value_or(10)), vars);
  });

  luaCtx.writeFunction("getTopCacheHitResponseRules", [](boost::optional<unsigned int> top) {
    setLuaNoSideEffect();
    auto rules = g_cachehitrespruleactions.getLocal();
    return getTopRules(*rules, top.get_value_or(10));
  });

  luaCtx.writeFunction("topCacheHitResponseRules", [](boost::optional<unsigned int> top, boost::optional<ruleparams_t> vars) {
    setLuaNoSideEffect();
    auto rules = g_cachehitrespruleactions.getLocal();
    return rulesToString(getTopRules(*rules, top.get_value_or(10)), vars);
  });

  luaCtx.writeFunction("getTopCacheInsertedResponseRules", [](boost::optional<unsigned int> top) {
    setLuaNoSideEffect();
    auto rules = g_cacheInsertedRespRuleActions.getLocal();
    return getTopRules(*rules, top.get_value_or(10));
  });

  luaCtx.writeFunction("topCacheInsertedResponseRules", [](boost::optional<unsigned int> top, boost::optional<ruleparams_t> vars) {
    setLuaNoSideEffect();
    auto rules = g_cacheInsertedRespRuleActions.getLocal();
    return rulesToString(getTopRules(*rules, top.get_value_or(10)), vars);
  });

  luaCtx.writeFunction("getTopResponseRules", [](boost::optional<unsigned int> top) {
    setLuaNoSideEffect();
    auto rules = g_respruleactions.getLocal();
    return getTopRules(*rules, top.get_value_or(10));
  });

  luaCtx.writeFunction("topResponseRules", [](boost::optional<unsigned int> top, boost::optional<ruleparams_t> vars) {
    setLuaNoSideEffect();
    auto rules = g_respruleactions.getLocal();
    return rulesToString(getTopRules(*rules, top.get_value_or(10)), vars);
  });

  luaCtx.writeFunction("getTopSelfAnsweredResponseRules", [](boost::optional<unsigned int> top) {
    setLuaNoSideEffect();
    auto rules = g_selfansweredrespruleactions.getLocal();
    return getTopRules(*rules, top.get_value_or(10));
  });

  luaCtx.writeFunction("topSelfAnsweredResponseRules", [](boost::optional<unsigned int> top, boost::optional<ruleparams_t> vars) {
    setLuaNoSideEffect();
    auto rules = g_selfansweredrespruleactions.getLocal();
    return rulesToString(getTopRules(*rules, top.get_value_or(10)), vars);
  });

  luaCtx.writeFunction("MaxQPSIPRule", [](unsigned int qps, boost::optional<unsigned int> ipv4trunc, boost::optional<unsigned int> ipv6trunc, boost::optional<unsigned int> burst, boost::optional<unsigned int> expiration, boost::optional<unsigned int> cleanupDelay, boost::optional<unsigned int> scanFraction) {
      return std::shared_ptr<DNSRule>(new MaxQPSIPRule(qps, burst.get_value_or(qps), ipv4trunc.get_value_or(32), ipv6trunc.get_value_or(64), expiration.get_value_or(300), cleanupDelay.get_value_or(60), scanFraction.get_value_or(10)));
    });

  luaCtx.writeFunction("MaxQPSRule", [](unsigned int qps, boost::optional<unsigned int> burst) {
      if(!burst)
        return std::shared_ptr<DNSRule>(new MaxQPSRule(qps));
      else
        return std::shared_ptr<DNSRule>(new MaxQPSRule(qps, *burst));
    });

  luaCtx.writeFunction("RegexRule", [](const std::string& str) {
      return std::shared_ptr<DNSRule>(new RegexRule(str));
    });

#ifdef HAVE_DNS_OVER_HTTPS
  luaCtx.writeFunction("HTTPHeaderRule", [](const std::string& header, const std::string& regex) {
      return std::shared_ptr<DNSRule>(new HTTPHeaderRule(header, regex));
    });
  luaCtx.writeFunction("HTTPPathRule", [](const std::string& path) {
      return std::shared_ptr<DNSRule>(new HTTPPathRule(path));
    });
  luaCtx.writeFunction("HTTPPathRegexRule", [](const std::string& regex) {
      return std::shared_ptr<DNSRule>(new HTTPPathRegexRule(regex));
    });
#endif

#ifdef HAVE_RE2
  luaCtx.writeFunction("RE2Rule", [](const std::string& str) {
      return std::shared_ptr<DNSRule>(new RE2Rule(str));
    });
#endif

  luaCtx.writeFunction("SNIRule", [](const std::string& name) {
      return std::shared_ptr<DNSRule>(new SNIRule(name));
  });

  luaCtx.writeFunction("SuffixMatchNodeRule", [](const SuffixMatchNode& smn, boost::optional<bool> quiet) {
      return std::shared_ptr<DNSRule>(new SuffixMatchNodeRule(smn, quiet ? *quiet : false));
    });

  luaCtx.writeFunction("NetmaskGroupRule", [](const NetmaskGroup& nmg, boost::optional<bool> src, boost::optional<bool> quiet) {
      return std::shared_ptr<DNSRule>(new NetmaskGroupRule(nmg, src ? *src : true, quiet ? *quiet : false));
    });

  luaCtx.writeFunction("benchRule", [](std::shared_ptr<DNSRule> rule, boost::optional<unsigned int> times_, boost::optional<string> suffix_)  {
      setLuaNoSideEffect();
      unsigned int times = times_.get_value_or(100000);
      DNSName suffix(suffix_.get_value_or("powerdns.com"));
      struct item {
        PacketBuffer packet;
        InternalQueryState ids;
      };
      vector<item> items;
      items.reserve(1000);
      for (int n = 0; n < 1000; ++n) {
        struct item i;
        i.ids.qname = DNSName(std::to_string(random()));
        i.ids.qname += suffix;
        i.ids.qtype = random() % 0xff;
        i.ids.qclass = QClass::IN;
        i.ids.protocol = dnsdist::Protocol::DoUDP;
        i.ids.origRemote = ComboAddress("127.0.0.1");
        i.ids.origRemote.sin4.sin_addr.s_addr = random();
        i.ids.queryRealTime.start();
        GenericDNSPacketWriter<PacketBuffer> pw(i.packet, i.ids.qname, i.ids.qtype);
        items.push_back(std::move(i));
      }

      int matches = 0;
      ComboAddress dummy("127.0.0.1");
      StopWatch sw;
      sw.start();
      for (unsigned int n = 0; n < times; ++n) {
        item& i = items[n % items.size()];
        DNSQuestion dq(i.ids, i.packet);

        if (rule->matches(&dq)) {
          matches++;
        }
      }
      double udiff = sw.udiff();
      g_outputBuffer=(boost::format("Had %d matches out of %d, %.1f qps, in %.1f usec\n") % matches % times % (1000000*(1.0*times/udiff)) % udiff).str();

    });

  luaCtx.writeFunction("AllRule", []() {
      return std::shared_ptr<DNSRule>(new AllRule());
    });

  luaCtx.writeFunction("ProbaRule", [](double proba) {
      return std::shared_ptr<DNSRule>(new ProbaRule(proba));
    });

  luaCtx.writeFunction("QNameRule", [](const std::string& qname) {
      return std::shared_ptr<DNSRule>(new QNameRule(DNSName(qname)));
    });

  luaCtx.writeFunction("QTypeRule", [](boost::variant<unsigned int, std::string> str) {
      uint16_t qtype;
      if (auto dir = boost::get<unsigned int>(&str)) {
        qtype = *dir;
      }
      else {
        string val = boost::get<string>(str);
        qtype = QType::chartocode(val.c_str());
        if (!qtype) {
          throw std::runtime_error("Unable to convert '"+val+"' to a DNS type");
        }
      }
      return std::shared_ptr<DNSRule>(new QTypeRule(qtype));
    });

  luaCtx.writeFunction("QClassRule", [](uint64_t c) {
      checkParameterBound("QClassRule", c, std::numeric_limits<uint16_t>::max());
      return std::shared_ptr<DNSRule>(new QClassRule(c));
    });

  luaCtx.writeFunction("OpcodeRule", [](uint64_t code) {
      checkParameterBound("OpcodeRule", code, std::numeric_limits<uint8_t>::max());
      return std::shared_ptr<DNSRule>(new OpcodeRule(code));
    });

  luaCtx.writeFunction("AndRule", [](LuaArray<std::shared_ptr<DNSRule>> a) {
      return std::shared_ptr<DNSRule>(new AndRule(a));
    });

  luaCtx.writeFunction("OrRule", [](LuaArray<std::shared_ptr<DNSRule>>a) {
      return std::shared_ptr<DNSRule>(new OrRule(a));
    });

  luaCtx.writeFunction("DSTPortRule", [](uint64_t port) {
      checkParameterBound("DSTPortRule", port, std::numeric_limits<uint16_t>::max());
      return std::shared_ptr<DNSRule>(new DSTPortRule(port));
    });

  luaCtx.writeFunction("TCPRule", [](bool tcp) {
      return std::shared_ptr<DNSRule>(new TCPRule(tcp));
    });

  luaCtx.writeFunction("DNSSECRule", []() {
      return std::shared_ptr<DNSRule>(new DNSSECRule());
    });

  luaCtx.writeFunction("NotRule", [](std::shared_ptr<DNSRule>rule) {
      return std::shared_ptr<DNSRule>(new NotRule(rule));
    });

  luaCtx.writeFunction("RecordsCountRule", [](uint64_t section, uint64_t minCount, uint64_t maxCount) {
      checkParameterBound("RecordsCountRule", section, std::numeric_limits<uint8_t>::max());
      checkParameterBound("RecordsCountRule", minCount, std::numeric_limits<uint16_t>::max());
      checkParameterBound("RecordsCountRule", maxCount, std::numeric_limits<uint16_t>::max());
      return std::shared_ptr<DNSRule>(new RecordsCountRule(section, minCount, maxCount));
    });

  luaCtx.writeFunction("RecordsTypeCountRule", [](uint64_t section, uint64_t type, uint64_t minCount, uint64_t maxCount) {
      checkParameterBound("RecordsTypeCountRule", section, std::numeric_limits<uint8_t>::max());
      checkParameterBound("RecordsTypeCountRule", type, std::numeric_limits<uint16_t>::max());
      checkParameterBound("RecordsTypeCountRule", minCount, std::numeric_limits<uint16_t>::max());
      checkParameterBound("RecordsTypeCountRule", maxCount, std::numeric_limits<uint16_t>::max());
      return std::shared_ptr<DNSRule>(new RecordsTypeCountRule(section, type, minCount, maxCount));
    });

  luaCtx.writeFunction("TrailingDataRule", []() {
      return std::shared_ptr<DNSRule>(new TrailingDataRule());
    });

  luaCtx.writeFunction("QNameLabelsCountRule", [](uint64_t minLabelsCount, uint64_t maxLabelsCount) {
      checkParameterBound("QNameLabelsCountRule", minLabelsCount, std::numeric_limits<unsigned int>::max());
      checkParameterBound("QNameLabelsCountRule", maxLabelsCount, std::numeric_limits<unsigned int>::max());
      return std::shared_ptr<DNSRule>(new QNameLabelsCountRule(minLabelsCount, maxLabelsCount));
    });

  luaCtx.writeFunction("QNameWireLengthRule", [](uint64_t min, uint64_t max) {
      return std::shared_ptr<DNSRule>(new QNameWireLengthRule(min, max));
    });

  luaCtx.writeFunction("RCodeRule", [](uint64_t rcode) {
      checkParameterBound("RCodeRule", rcode, std::numeric_limits<uint8_t>::max());
      return std::shared_ptr<DNSRule>(new RCodeRule(rcode));
    });

  luaCtx.writeFunction("ERCodeRule", [](uint64_t rcode) {
      checkParameterBound("ERCodeRule", rcode, std::numeric_limits<uint8_t>::max());
      return std::shared_ptr<DNSRule>(new ERCodeRule(rcode));
    });

  luaCtx.writeFunction("EDNSVersionRule", [](uint64_t version) {
      checkParameterBound("EDNSVersionRule", version, std::numeric_limits<uint8_t>::max());
      return std::shared_ptr<DNSRule>(new EDNSVersionRule(version));
    });

  luaCtx.writeFunction("EDNSOptionRule", [](uint64_t optcode) {
      checkParameterBound("EDNSOptionRule", optcode, std::numeric_limits<uint16_t>::max());
      return std::shared_ptr<DNSRule>(new EDNSOptionRule(optcode));
    });

  luaCtx.writeFunction("showRules", [](boost::optional<ruleparams_t> vars) {
      showRules(&g_ruleactions, vars);
    });

  luaCtx.writeFunction("RDRule", []() {
      return std::shared_ptr<DNSRule>(new RDRule());
    });

  luaCtx.writeFunction("TagRule", [](std::string tag, boost::optional<std::string> value) {
      return std::shared_ptr<DNSRule>(new TagRule(tag, value));
    });

  luaCtx.writeFunction("TimedIPSetRule", []() {
      return std::shared_ptr<TimedIPSetRule>(new TimedIPSetRule());
    });

  luaCtx.writeFunction("PoolAvailableRule", [](std::string poolname) {
    return std::shared_ptr<DNSRule>(new PoolAvailableRule(poolname));
  });

  luaCtx.writeFunction("PoolOutstandingRule", [](std::string poolname, uint64_t limit) {
    return std::shared_ptr<DNSRule>(new PoolOutstandingRule(poolname, limit));
  });

  luaCtx.registerFunction<void(std::shared_ptr<TimedIPSetRule>::*)()>("clear", [](std::shared_ptr<TimedIPSetRule> tisr) {
      tisr->clear();
    });

  luaCtx.registerFunction<void(std::shared_ptr<TimedIPSetRule>::*)()>("cleanup", [](std::shared_ptr<TimedIPSetRule> tisr) {
      tisr->cleanup();
    });

  luaCtx.registerFunction<void(std::shared_ptr<TimedIPSetRule>::*)(const ComboAddress& ca, int t)>("add", [](std::shared_ptr<TimedIPSetRule> tisr, const ComboAddress& ca, int t) {
      tisr->add(ca, time(0)+t);
    });

  luaCtx.registerFunction<std::shared_ptr<DNSRule>(std::shared_ptr<TimedIPSetRule>::*)()>("slice", [](std::shared_ptr<TimedIPSetRule> tisr) {
      return std::dynamic_pointer_cast<DNSRule>(tisr);
    });
  luaCtx.registerFunction<void(std::shared_ptr<TimedIPSetRule>::*)()>("__tostring", [](std::shared_ptr<TimedIPSetRule> tisr) {
      tisr->toString();
    });

  luaCtx.writeFunction("QNameSetRule", [](const DNSNameSet& names) {
      return std::shared_ptr<DNSRule>(new QNameSetRule(names));
    });

#if defined(HAVE_LMDB) || defined(HAVE_CDB)
  luaCtx.writeFunction("KeyValueStoreLookupRule", [](std::shared_ptr<KeyValueStore>& kvs, std::shared_ptr<KeyValueLookupKey>& lookupKey) {
      return std::shared_ptr<DNSRule>(new KeyValueStoreLookupRule(kvs, lookupKey));
    });

  luaCtx.writeFunction("KeyValueStoreRangeLookupRule", [](std::shared_ptr<KeyValueStore>& kvs, std::shared_ptr<KeyValueLookupKey>& lookupKey) {
      return std::shared_ptr<DNSRule>(new KeyValueStoreRangeLookupRule(kvs, lookupKey));
    });
#endif /* defined(HAVE_LMDB) || defined(HAVE_CDB) */

  luaCtx.writeFunction("LuaRule", [](LuaRule::func_t func) {
      return std::shared_ptr<DNSRule>(new LuaRule(func));
    });

  luaCtx.writeFunction("LuaFFIRule", [](LuaFFIRule::func_t func) {
      return std::shared_ptr<DNSRule>(new LuaFFIRule(func));
    });

  luaCtx.writeFunction("LuaFFIPerThreadRule", [](std::string code) {
    return std::shared_ptr<DNSRule>(new LuaFFIPerThreadRule(code));
  });

  luaCtx.writeFunction("ProxyProtocolValueRule", [](uint8_t type, boost::optional<std::string> value) {
      return std::shared_ptr<DNSRule>(new ProxyProtocolValueRule(type, value));
    });
}

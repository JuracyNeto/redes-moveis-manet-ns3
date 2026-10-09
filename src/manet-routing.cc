/*
 * manet-routing.cc — Avaliação de AODV, OLSR e DSDV em uma MANET sob mobilidade.
 *
 * Trabalho final de Redes Móveis (IFG Câmpus Inhumas, 2026/2).
 * Referência de estrutura: examples/routing/manet-routing-compare.cc (ns-3.48),
 * reescrito para o cenário do enunciado e para medir PDR, throughput, atraso e
 * jitter por fluxo de dados.
 *
 * Execução (a partir de ~/ns-3):
 *   ./ns3 run "manet-routing --protocol=AODV --speed=10 --run=1 --outFile=res.csv"
 *
 * Cada execução simula UM cenário (protocolo, velocidade, run) e ANEXA uma linha
 * por fluxo de dados ao CSV indicado em --outFile.
 *
 * Linha do tempo de uma execução (valores padrão):
 *
 *   0 s ─────────── 20 s ──────────────────────────────── 115 s ─── 120 s
 *   |  aquecimento   |       tráfego CBR (95 s)            | escoamento |
 *   |  (rotas)       |       janela do throughput          | (drain)    |
 */

#include "ns3/aodv-module.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/dsdv-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/olsr-module.h"
#include "ns3/wifi-module.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

using namespace ns3;

// =============================================================================
// Aplicação CBR (Constant Bit Rate) sobre UDP
// -----------------------------------------------------------------------------
// Por que não usar o OnOffApplication do ns-3?
//   Quando a origem não tem rota (comum em OLSR e DSDV sob mobilidade), o socket
//   UDP recusa o pacote ANTES da camada IP (Send() retorna -1). Consequências:
//     - o FlowMonitor, que observa a camada IP, nunca vê esse pacote, então o
//       txPackets dele fica subestimado e o PDR, superestimado;
//     - o OnOffApplication guarda o pacote recusado e o reenvia depois, o que
//       também esconde a perda.
//   O AODV, ao contrário, aceita o pacote e o segura enquanto descobre a rota;
//   então o FlowMonitor o conta. Usar o txPackets do FlowMonitor tornaria a
//   comparação entre protocolos injusta.
//
//   Esta aplicação conta TODA tentativa de envio (carga oferecida pela
//   aplicação), tenha a camada de rede aceitado o pacote ou não. Esse contador é
//   o denominador do PDR, igual para os três protocolos.
// =============================================================================
class CbrSender : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("CbrSender")
                                .SetParent<Application>()
                                .SetGroupName("Applications")
                                .AddConstructor<CbrSender>();
        return tid;
    }

    // peer: destino (IP:porta); pktSize: carga UDP em bytes;
    // interval: intervalo entre pacotes; stop: instante após o qual não envia.
    void Setup(Address peer, uint32_t pktSize, Time interval, Time stop)
    {
        m_peer = peer;
        m_pktSize = pktSize;
        m_interval = interval;
        m_stop = stop;
    }

    uint32_t GetAttempts() const { return m_attempts; }  // tentativas (carga oferecida)
    uint32_t GetAccepted() const { return m_accepted; }  // aceitas pela camada de rede

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind();
        m_socket->Connect(m_peer);
        SendPacket(); // primeiro pacote exatamente no instante de início
    }

    void StopApplication() override
    {
        Simulator::Cancel(m_sendEvent);
        if (m_socket)
        {
            m_socket->Close();
        }
    }

    void SendPacket()
    {
        m_attempts++;
        // Send() < 0 significa que a pilha recusou o pacote (ex.: sem rota).
        // A tentativa conta como transmitida; a perda aparecerá no PDR.
        if (m_socket->Send(Create<Packet>(m_pktSize)) >= 0)
        {
            m_accepted++;
        }
        // Agenda o próximo envio só se ele cair ANTES do fim da janela. Assim o
        // número de pacotes é exato e não depende da ordem de eventos simultâneos.
        if (Simulator::Now() + m_interval < m_stop)
        {
            m_sendEvent = Simulator::Schedule(m_interval, &CbrSender::SendPacket, this);
        }
    }

    Ptr<Socket> m_socket;
    Address m_peer;
    uint32_t m_pktSize{0};
    Time m_interval;
    Time m_stop;
    EventId m_sendEvent;
    uint32_t m_attempts{0};
    uint32_t m_accepted{0};
};

NS_OBJECT_ENSURE_REGISTERED(CbrSender);

// Métricas acumuladas de um fluxo de dados da aplicação.
struct FlowResult
{
    Ipv4Address src;
    Ipv4Address dst;
    Ptr<CbrSender> app;
    uint64_t rxPackets{0};
    uint64_t rxBytes{0};
    uint64_t fmTxPackets{0}; // txPackets segundo o FlowMonitor (só diagnóstico)
    Time delaySum;
    Time jitterSum;
};

// Formata um valor para o CSV; "NA" quando a métrica não está definida
// (ex.: atraso de um fluxo que não recebeu nenhum pacote). O pandas lê "NA"
// como valor ausente, então a análise não confunde "indefinido" com zero.
static std::string
Fmt(double value, bool defined, int precision)
{
    if (!defined)
    {
        return "NA";
    }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(precision) << value;
    return ss.str();
}

int
main(int argc, char* argv[])
{
    // -------------------------------------------------------------------------
    // 1. Parâmetros (todos via linha de comando)
    // Tudo o que pode variar entre experimentos é argumento: o lote (Fase 3) e a
    // variável adicional do grupo rodam sem recompilar. Os padrões reproduzem o
    // cenário-base do enunciado.
    // -------------------------------------------------------------------------
    std::string protocol = "AODV";
    double speed = 10.0;          // m/s, velocidade FIXA dos nós
    uint32_t run = 1;             // número da repetição (substream do RNG)
    double simTime = 120.0;       // s
    uint32_t nodes = 20;
    uint32_t flows = 4;
    uint32_t packetSize = 1024;   // bytes de carga UDP
    double pauseTime = 0.0;       // s, pausa no Random Waypoint
    double areaSize = 500.0;      // m, lado da área quadrada
    double pktRate = 4.0;         // pacotes/s por fluxo
    double txPower = 6.0;         // dBm (alcance de ~250 m; ver seção 4)
    std::string propagation = "Friis";
    double warmup = 20.0;         // s, aquecimento antes do tráfego
    double drain = 5.0;           // s, escoamento após o tráfego
    std::string outFile = "manet-routing.csv";
    std::string routesFile = "";  // se definido, grava as tabelas de rota

    CommandLine cmd(__FILE__);
    cmd.AddValue("protocol", "Protocolo de roteamento: AODV, OLSR ou DSDV", protocol);
    cmd.AddValue("speed", "Velocidade fixa dos nós (m/s, > 0)", speed);
    cmd.AddValue("run", "Número da repetição (RngSeedManager::SetRun)", run);
    cmd.AddValue("simTime", "Duração total da simulação (s)", simTime);
    cmd.AddValue("nodes", "Número de nós", nodes);
    cmd.AddValue("flows", "Número de fluxos CBR simultâneos", flows);
    cmd.AddValue("packetSize", "Tamanho da carga UDP (bytes)", packetSize);
    cmd.AddValue("pauseTime", "Tempo de pausa do Random Waypoint (s)", pauseTime);
    cmd.AddValue("areaSize", "Lado da área quadrada (m)", areaSize);
    cmd.AddValue("pktRate", "Taxa de envio de cada fluxo (pacotes/s)", pktRate);
    cmd.AddValue("txPower", "Potência de transmissão (dBm)", txPower);
    cmd.AddValue("propagation", "Modelo de perda de propagação: Friis ou LogDistance", propagation);
    cmd.AddValue("warmup", "Aquecimento antes do início do tráfego (s)", warmup);
    cmd.AddValue("drain", "Escoamento entre o fim do tráfego e o fim da simulação (s)", drain);
    cmd.AddValue("outFile", "CSV de saída (linhas são anexadas)", outFile);
    cmd.AddValue("routesFile", "Arquivo para as tabelas de rota (vazio = não grava)", routesFile);
    cmd.Parse(argc, argv);

    // Validação: falhar cedo com mensagem clara em vez de produzir dados sem sentido.
    if (protocol != "AODV" && protocol != "OLSR" && protocol != "DSDV")
    {
        NS_FATAL_ERROR("Protocolo inválido: " << protocol << " (use AODV, OLSR ou DSDV)");
    }
    if (speed <= 0)
    {
        // Com velocidade 0 o Random Waypoint nunca chegaria ao destino.
        NS_FATAL_ERROR("--speed deve ser > 0");
    }
    if (flows == 0 || 2 * flows > nodes)
    {
        // Cada fluxo usa dois nós exclusivos (origem e destino distintos).
        NS_FATAL_ERROR("--flows deve estar entre 1 e nodes/2");
    }
    if (pktRate <= 0 || run == 0 || warmup + drain >= simTime)
    {
        NS_FATAL_ERROR("Parâmetros temporais/taxa inválidos");
    }
    if (propagation != "Friis" && propagation != "LogDistance")
    {
        NS_FATAL_ERROR("--propagation deve ser Friis ou LogDistance");
    }

    // -------------------------------------------------------------------------
    // 2. Aleatoriedade: seed mestre fixa + run variável (slide 22)
    // Cada run usa uma substream independente do mesmo gerador. Com seed e run
    // iguais, a simulação é determinística (reprodutível bit a bit).
    // -------------------------------------------------------------------------
    const uint32_t seed = 12345;
    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);

    // -------------------------------------------------------------------------
    // 3. Nós
    // -------------------------------------------------------------------------
    NodeContainer adhocNodes;
    adhocNodes.Create(nodes);

    // -------------------------------------------------------------------------
    // 4. Wi-Fi IEEE 802.11b em modo Ad Hoc
    //   - Padrão 802.11b, taxa FIXA de 11 Mb/s (DSSS/CCK), sem adaptação de taxa
    //     (ConstantRateWifiManager). Isola o efeito do roteamento: com adaptação,
    //     a taxa mudaria com a distância e misturaria dois efeitos.
    //   - Mesma taxa para quadros broadcast (NonUnicastMode). Os pacotes de
    //     controle dos protocolos são broadcast; por padrão iriam a 1 Mb/s, com
    //     alcance MAIOR que o dos dados. Haveria vizinhos "fantasmas": rotas
    //     descobertas pelo controle que não funcionam para dados.
    //   - Canal Yans com atraso de propagação à velocidade da luz e perda Friis
    //     (espaço livre) a 2,412 GHz (canal 1 do 802.11b).
    //   - Potência de 6 dBm: alcance de ~250 m. O receptor só detecta quadros com
    //     RSSI >= -82 dBm (modelo de detecção de preâmbulo padrão do ns-3), e a
    //     perda Friis atinge 6 - (-82) = 88 dB em ~248 m. Medido com 2 nós
    //     parados: 100% de entrega até 245 m e 0% a partir de 250 m, igual para
    //     unicast (1024 B) e broadcast (64 B). 250 m é o alcance nominal clássico
    //     dos estudos de MANET (Broch et al., 1998), o que facilita comparar com a
    //     literatura. Numa área de 500 x 500 m (diagonal de 707 m), há rotas de
    //     múltiplos saltos, o que interessa estudar.
    // -------------------------------------------------------------------------
    const std::string phyMode = "DsssRate11Mbps";
    Config::SetDefault("ns3::WifiRemoteStationManager::NonUnicastMode", StringValue(phyMode));

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211b);
    wifi.SetRemoteStationManager("ns3::ConstantRateWifiManager",
                                 "DataMode", StringValue(phyMode),
                                 "ControlMode", StringValue(phyMode));

    // ATENÇÃO: os modelos de perda do ns-3 NÃO herdam a frequência do Wi-Fi.
    // O Friis usa 5,15 GHz por padrão e o LogDistance tem perda de referência
    // calculada para 5,15 GHz. Com 802.11b (canal 1 = 2,412 GHz), isso
    // subestimaria o alcance em mais da metade (medido com 7,5 dBm: 135 m em vez
    // de ~295 m). O exemplo oficial manet-routing-compare tem essa inconsistência.
    // Por isso a frequência é fixada explicitamente, para coerência física.
    const double frequencyHz = 2.412e9;
    // Perda em espaço livre a 1 m: 20*log10(4*pi*f/c) = 40,09 dB a 2,412 GHz.
    const double refLoss1m = 20.0 * std::log10(4.0 * M_PI * frequencyHz / 299792458.0);

    YansWifiChannelHelper wifiChannel;
    wifiChannel.SetPropagationDelay("ns3::ConstantSpeedPropagationDelayModel");
    if (propagation == "Friis")
    {
        wifiChannel.AddPropagationLoss("ns3::FriisPropagationLossModel",
                                       "Frequency", DoubleValue(frequencyHz));
    }
    else
    {
        // Alternativa mais pessimista (expoente 3), para a variável adicional.
        wifiChannel.AddPropagationLoss("ns3::LogDistancePropagationLossModel",
                                       "ReferenceLoss", DoubleValue(refLoss1m));
    }

    YansWifiPhyHelper wifiPhy;
    wifiPhy.SetChannel(wifiChannel.Create());
    wifiPhy.Set("TxPowerStart", DoubleValue(txPower));
    wifiPhy.Set("TxPowerEnd", DoubleValue(txPower));

    WifiMacHelper wifiMac;
    wifiMac.SetType("ns3::AdhocWifiMac"); // sem Access Point: nós falam diretamente
    NetDeviceContainer devices = wifi.Install(wifiPhy, wifiMac, adhocNodes);

    // -------------------------------------------------------------------------
    // 5. Mobilidade: Random Waypoint com velocidade FIXA
    //   - Posições iniciais e destinos uniformes na área [0, areaSize]^2.
    //   - Velocidade constante (equivale a Uniform[Min=speed|Max=speed]). Com uma
    //     faixa larga, como U(0, 20), a velocidade MÉDIA cai ao longo do tempo:
    //     nós lentos levam mais tempo para cruzar a área e passam a dominar a
    //     amostra (Yoon, Liu e Noble, 2003). Com velocidade fixa, "speed=10"
    //     significa 10 m/s durante toda a simulação.
    //   - AssignStreams com índices FIXOS: a mobilidade usa substreams próprias do
    //     gerador. Assim, para o mesmo run, as trajetórias são IDÊNTICAS nos três
    //     protocolos, mesmo que cada protocolo sorteie quantidades diferentes de
    //     números aleatórios ("regra de ouro" do slide 21: mesmas condições de
    //     mobilidade). Sem isso, a numeração automática das streams dependeria do
    //     protocolo e as trajetórias mudariam junto.
    // -------------------------------------------------------------------------
    std::ostringstream uniformArea;
    uniformArea << "ns3::UniformRandomVariable[Min=0.0|Max=" << areaSize << "]";
    ObjectFactory posFactory;
    posFactory.SetTypeId("ns3::RandomRectanglePositionAllocator");
    posFactory.Set("X", StringValue(uniformArea.str()));
    posFactory.Set("Y", StringValue(uniformArea.str()));
    Ptr<PositionAllocator> positionAlloc = posFactory.Create()->GetObject<PositionAllocator>();

    int64_t mobilityStream = 0;
    mobilityStream += positionAlloc->AssignStreams(mobilityStream);

    std::ostringstream speedVar;
    speedVar << "ns3::ConstantRandomVariable[Constant=" << speed << "]";
    std::ostringstream pauseVar;
    pauseVar << "ns3::ConstantRandomVariable[Constant=" << pauseTime << "]";

    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::RandomWaypointMobilityModel",
                              "Speed", StringValue(speedVar.str()),
                              "Pause", StringValue(pauseVar.str()),
                              "PositionAllocator", PointerValue(positionAlloc));
    mobility.SetPositionAllocator(positionAlloc);
    mobility.Install(adhocNodes);
    mobilityStream += mobility.AssignStreams(adhocNodes, mobilityStream);

    // -------------------------------------------------------------------------
    // 6. Pilha IP + protocolo de roteamento (estrutura do slide 16)
    // Um único programa instala o protocolo pedido em --protocol; nada mais
    // muda entre os três.
    // -------------------------------------------------------------------------
    InternetStackHelper internet;
    AodvHelper aodv;
    OlsrHelper olsr;
    DsdvHelper dsdv;
    if (protocol == "AODV")
    {
        internet.SetRoutingHelper(aodv);
    }
    else if (protocol == "OLSR")
    {
        internet.SetRoutingHelper(olsr);
    }
    else
    {
        internet.SetRoutingHelper(dsdv);
    }
    internet.Install(adhocNodes);

    Ipv4AddressHelper address;
    address.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer interfaces = address.Assign(devices);

    // -------------------------------------------------------------------------
    // 7. Tráfego: fluxos UDP CBR com pares origem/destino distintos e fixos
    //   - Fluxo i: nó i -> nó (nodes-1-i). Com 20 nós e 4 fluxos:
    //     0->19, 1->18, 2->17, 3->16. Oito nós distintos; nenhum nó participa de
    //     dois fluxos. Como as posições são aleatórias, índices fixos não
    //     favorecem nenhum par.
    //   - Taxa: 4 pacotes/s x 1024 B = 32,8 kb/s por fluxo, ~131 kb/s no total
    //     (~135 kb/s com cabeçalhos IP/UDP). O canal de 11 Mb/s entrega na
    //     prática ~5–6 Mb/s em um salto; mesmo dividido por rotas de vários
    //     saltos, a carga fica bem abaixo da saturação. Assim, as perdas medidas
    //     refletem mobilidade e roteamento, não congestionamento. 4 pacotes/s é a
    //     taxa clássica de Broch et al. (1998). A Fase 2 valida isso
    //     empiricamente (PDR ~100% com mobilidade baixa).
    //   - Início após o aquecimento (padrão de 20 s): os protocolos PROATIVOS
    //     precisam de tempo para montar as tabelas. OLSR: HELLO a cada 2 s, TC a
    //     cada 5 s; a topologia completa exige algumas rodadas de TC. DSDV:
    //     atualização periódica completa a cada 15 s. Sem aquecimento, os
    //     primeiros segundos mediriam a convergência inicial, não o efeito da
    //     mobilidade, e prejudicariam os proativos de forma artificial. O AODV
    //     (reativo) não precisa disso, mas usa a mesma janela, para os três serem
    //     comparados em condições idênticas.
    //   - Inícios defasados em 0,1 s entre fluxos, para as quatro origens não
    //     dispararem descobertas de rota (broadcast, sem ACK) no mesmo instante.
    //   - Todos os fluxos têm a MESMA duração de tráfego, usada no throughput.
    //   - Escoamento (padrão de 5 s): pacotes enviados nos últimos instantes
    //     precisam de tempo para chegar. Sem isso, seriam contados como perdidos
    //     só porque a simulação terminou.
    // -------------------------------------------------------------------------
    const uint16_t appPort = 9000;
    const double stagger = 0.1;
    const double trafficDuration = simTime - warmup - drain - stagger * (flows - 1);
    const Time interval = Seconds(1.0 / pktRate);

    PacketSinkHelper sinkHelper("ns3::UdpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), appPort));

    std::vector<FlowResult> results(flows);
    for (uint32_t i = 0; i < flows; i++)
    {
        uint32_t srcIdx = i;
        uint32_t dstIdx = nodes - 1 - i;
        results[i].src = interfaces.GetAddress(srcIdx);
        results[i].dst = interfaces.GetAddress(dstIdx);

        // O sink consome os pacotes no destino. Sem ele, cada pacote geraria
        // uma mensagem ICMP "porta inalcançável", tráfego extra que não interessa.
        ApplicationContainer sinkApp = sinkHelper.Install(adhocNodes.Get(dstIdx));
        sinkApp.Start(Seconds(0.0));

        double start = warmup + stagger * i;
        double stop = start + trafficDuration;
        Ptr<CbrSender> sender = CreateObject<CbrSender>();
        sender->Setup(InetSocketAddress(results[i].dst, appPort), packetSize, interval,
                      Seconds(stop));
        adhocNodes.Get(srcIdx)->AddApplication(sender);
        sender->SetStartTime(Seconds(start));
        sender->SetStopTime(Seconds(stop));
        results[i].app = sender;
    }

    // -------------------------------------------------------------------------
    // 8. Instrumentação: FlowMonitor + tabelas de rota (opcional)
    // As tabelas de rota servem para validar que o protocolo está instalado:
    //   - no fim do aquecimento: os proativos (OLSR/DSDV) já devem ter rotas
    //     para quase todos os nós; o AODV, reativo, ainda não tem nenhuma;
    //   - 10 s após o início do tráfego: o AODV já criou rotas sob demanda.
    // -------------------------------------------------------------------------
    FlowMonitorHelper flowmonHelper;
    Ptr<FlowMonitor> monitor = flowmonHelper.InstallAll();

    if (!routesFile.empty())
    {
        Ptr<OutputStreamWrapper> routesStream = Create<OutputStreamWrapper>(routesFile, std::ios::out);
        Ipv4RoutingHelper::PrintRoutingTableAllAt(Seconds(warmup), routesStream);
        Ipv4RoutingHelper::PrintRoutingTableAllAt(Seconds(warmup + 10.0), routesStream);
    }

    // -------------------------------------------------------------------------
    // 9. Execução
    // -------------------------------------------------------------------------
    Simulator::Stop(Seconds(simTime));
    Simulator::Run();

    // -------------------------------------------------------------------------
    // 10. Coleta: apenas os fluxos de DADOS da aplicação
    // O FlowMonitor classifica como "fluxo" todo tráfego IP, inclusive o
    // controle dos protocolos de roteamento (UDP 654 = AODV, 698 = OLSR,
    // 269 = DSDV). Filtramos pela porta da aplicação (UDP 9000) e associamos
    // cada fluxo do FlowMonitor ao nosso fluxo pelo par (IP origem, IP destino).
    // O flowId do CSV é o índice do NOSSO fluxo (1..flows), estável entre runs;
    // o FlowId interno do FlowMonitor depende da ordem do primeiro pacote.
    // -------------------------------------------------------------------------
    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(flowmonHelper.GetClassifier());
    std::map<uint16_t, uint32_t> excludedByPort; // diagnóstico: fluxos ignorados

    for (const auto& [fmId, st] : monitor->GetFlowStats())
    {
        Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow(fmId);
        if (t.protocol != 17 || t.destinationPort != appPort)
        {
            excludedByPort[t.destinationPort]++;
            continue;
        }
        for (auto& r : results)
        {
            if (r.src == t.sourceAddress && r.dst == t.destinationAddress)
            {
                r.rxPackets += st.rxPackets;
                r.rxBytes += st.rxBytes;
                r.fmTxPackets += st.txPackets;
                r.delaySum += st.delaySum;
                r.jitterSum += st.jitterSum;
            }
        }
    }

    // -------------------------------------------------------------------------
    // 11. Métricas e CSV
    //   txPackets    : tentativas de envio da aplicação (carga oferecida)
    //   rxPackets    : pacotes entregues ao destino (FlowMonitor)
    //   lostPackets  : txPackets - rxPackets (descartados na origem, no caminho
    //                  ou não entregues até o fim da simulação)
    //   rxBytes      : bytes recebidos, medidos na camada IP (FlowMonitor):
    //                  incluem 28 B de cabeçalhos IP+UDP por pacote
    //   throughput   : rxBytes x 8 / duração do tráfego do fluxo (Mb/s). A janela é
    //                  a mesma para todos os fluxos e protocolos (padrão: 94,7 s).
    //   PDR          : rxPackets / txPackets x 100 (%)
    //   delay médio  : delaySum / rxPackets (ms); indefinido se rx = 0
    //   jitter médio : jitterSum / (rxPackets - 1) (ms); indefinido se rx < 2
    //                  (o jitter é a variação entre pacotes consecutivos)
    // -------------------------------------------------------------------------
    std::ifstream probe(outFile);
    bool needHeader = !probe.good() || probe.peek() == std::ifstream::traits_type::eof();
    probe.close();

    std::ofstream csv(outFile, std::ios::app);
    if (!csv)
    {
        NS_FATAL_ERROR("Não foi possível abrir " << outFile);
    }
    if (needHeader)
    {
        csv << "protocol,speed,pauseTime,nodes,seed,run,flowId,txPackets,rxPackets,"
               "lostPackets,rxBytes,throughputMbps,pdrPct,delayMs,jitterMs\n";
    }

    std::cout << "== manet-routing | protocolo=" << protocol << " speed=" << speed
              << " m/s run=" << run << " seed=" << seed << "\n"
              << "   roteamento instalado (nó 0): "
              << adhocNodes.Get(0)->GetObject<Ipv4>()->GetRoutingProtocol()->GetInstanceTypeId().GetName()
              << "\n";

    for (uint32_t i = 0; i < flows; i++)
    {
        const FlowResult& r = results[i];
        uint64_t tx = r.app->GetAttempts();
        uint64_t rx = r.rxPackets;
        uint64_t lost = (tx >= rx) ? tx - rx : 0;
        double throughputMbps = r.rxBytes * 8.0 / trafficDuration / 1e6;
        double pdr = tx > 0 ? 100.0 * rx / tx : 0.0;
        double delayMs = rx > 0 ? r.delaySum.GetSeconds() * 1000.0 / rx : 0.0;
        double jitterMs = rx > 1 ? r.jitterSum.GetSeconds() * 1000.0 / (rx - 1) : 0.0;

        csv << protocol << "," << speed << "," << pauseTime << "," << nodes << "," << seed
            << "," << run << "," << (i + 1) << "," << tx << "," << rx << "," << lost << ","
            << r.rxBytes << "," << Fmt(throughputMbps, true, 6) << ","
            << Fmt(pdr, tx > 0, 4) << "," << Fmt(delayMs, rx > 0, 4) << ","
            << Fmt(jitterMs, rx > 1, 4) << "\n";

        std::cout << "   fluxo " << (i + 1) << " " << r.src << "->" << r.dst << ": tx=" << tx
                  << " (aceitos pela rede=" << r.app->GetAccepted()
                  << ", vistos pelo FlowMonitor=" << r.fmTxPackets << ") rx=" << rx
                  << " PDR=" << Fmt(pdr, tx > 0, 2) << "% thr=" << Fmt(throughputMbps, true, 4)
                  << " Mb/s delay=" << Fmt(delayMs, rx > 0, 2)
                  << " ms jitter=" << Fmt(jitterMs, rx > 1, 2) << " ms\n";
    }

    std::cout << "   fluxos excluídos (porta destino: quantidade):";
    for (const auto& [port, count] : excludedByPort)
    {
        std::cout << " " << port << ":" << count;
    }
    std::cout << "\n";

    // "Assinatura" da mobilidade: soma das posições finais de todos os nós. Para
    // o mesmo run, ela deve ser IDÊNTICA nos três protocolos (ver seção 5).
    double sx = 0;
    double sy = 0;
    for (uint32_t n = 0; n < nodes; n++)
    {
        Vector p = adhocNodes.Get(n)->GetObject<MobilityModel>()->GetPosition();
        sx += p.x;
        sy += p.y;
    }
    std::cout << std::fixed << std::setprecision(6) << "   assinatura-mobilidade: " << sx << " "
              << sy << "\n";

    Simulator::Destroy();
    return 0;
}

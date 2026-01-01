//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/.
//

package clusteringmanet.simulations;

import inet.physicallayer.wireless.unitdisk.UnitDiskRadioMedium;
import inet.networklayer.configurator.ipv4.Ipv4NetworkConfigurator;
import clusteringmanet.simulations.GCCAdhocHost;

network ClusteringNetwork
{
    parameters:
        int numHosts = default(10);

        // --- GLOBAL STATISTICS ---
        // These subscribe to the signals emitted by ANY host in the network
        // Number of roles in the network
		@signal[chChange](type=long);
        @statistic[totalCHs](source=sum(chChange); record=vector,timeavg,last; title="Total Number of Cluster Heads");

        @signal[gwChange](type=long);
        @statistic[totalGWs](source=sum(gwChange); record=vector,timeavg,last; title="Total Number of Gateways");

        @signal[memberChange](type=long);
        @statistic[totalMembers](source=sum(memberChange); record=vector,timeavg,last; title="Total Number of Members");

        // Global PDR
        // 1. Sum up 'dataSent' from ALL hosts
        @statistic[globalSent](source=sum(dataSent); record=vector,last; title="Total Packets Sent by Network");

        // 2. Sum up 'dataReceived' from ALL hosts
        @statistic[globalReceived](source=sum(dataReceived); record=vector,last; title="Total Packets Received by Network");

		// Collect delay from ALL hosts and calculate the global average
        @statistic[globalDelay](source="delay"; record=vector,mean; title="Average End-to-End Delay");

        // 1. Sum up all bits received by all nodes
        @statistic[globalBits](source=sum(throughputBits); record=last,vector; title="Total Bits Received");

    submodules:
        radioMedium: UnitDiskRadioMedium;
        configurator: Ipv4NetworkConfigurator;
        host[numHosts]: GCCAdhocHost {
            @display("i=device/laptop");
        }
}

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
//import inet.applications.contract.IUDPApp;
import inet.applications.contract.IApp;
//
// TODO auto-generated type
//
simple GraphColoringClustering like IApp
{
    parameters:
        @display("i=block/app");

        // your existing parameters
        double helloInterval @unit(s);
        double helloJitter @unit(s);
        double neighborTimeout @unit(s);
        double maintenanceInterval @unit(s);
        double coloringInterval @unit(s);
        double dataInterval @unit(s);
        double dataJitter @unit(s);
		// NEW: UDP parameters
        int localPort = default(5000);
        int destPort = default(5000);

        // --- STATISTICS COLLECTION ---
        // 1. Define the signal carrying "long" (integer) values


        // 2. Define how to record it. 'vector' allows plotting over time.
        @signal[role](type="long");
        @statistic[role](source="role"; record=vector; interpolationmode=sample-hold);

        // 'count' will sum up the total number of packets sent/received across the simulation
		@signal[dataSent](type="long");
        @statistic[totalSent](source="dataSent"; record=count);

        @signal[dataReceived](type="long");
        @statistic[totalReceived](source="dataReceived"; record=count);

    gates:
        input socketIn @labels(UdpControlInfo/up);
        output socketOut @labels(UdpControlInfo/down);
}

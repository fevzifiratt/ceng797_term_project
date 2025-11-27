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

//
// TODO auto-generated type
//
import inet.node.inet.AdhocHost;
import clusteringmanet.simulations.GraphColoringClustering;

module GCCAdhocHost extends AdhocHost
{
    submodules:
        gcc: GraphColoringClustering {
        }
    //connections allowunconnected:
    //    gcc.out --> wlan[0].upperLayerIn;
    //    wlan[0].upperLayerOut --> gcc.in;
}

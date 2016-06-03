#include "frameworks/util.hpp"
#include "frameworks/task.hpp"
#include "frameworks/tensor.hpp"
#include "frameworks/logging.hpp"
#include "frameworks/operator.hpp"

using namespace aquarius::task;
using namespace aquarius::logging;
using namespace aquarius::symmetry;
using namespace aquarius::tensor;
using namespace aquarius::op;

namespace aquarius
{
namespace jellium
{

class Jellium : public task::Task
{
    protected:
        int nelec;
        int norb;
        double radius;
        vector<vec3> gvecs;
        int nocc;
        double V;
        double L;
        double PotVm;

        void writeIntegrals(bool pvirt, bool qvirt, bool rvirt, bool svirt,
                            Tensor<PGSYMMETRIC> tensor)
        {
            KeyValueVector pairs;
            tensor.getLocalData({0,0,0,0}, pairs);

            int np = (pvirt ? norb-nocc : nocc);
            int nq = (qvirt ? norb-nocc : nocc);
            int nr = (rvirt ? norb-nocc : nocc);
            int ns = (svirt ? norb-nocc : nocc);

            for (auto& pair : pairs)
            {
                auto k = pair.k;
                int p = k%np;
                k /= np;
                int q = k%nq;
                k /= nq;
                int r = k%nr;
                k /= nr;
                int s = k;

                if (pvirt) p += nocc;
                if (qvirt) q += nocc;
                if (rvirt) r += nocc;
                if (svirt) s += nocc;

                vec3 pr = gvecs[p]-gvecs[r];
                vec3 sq = gvecs[s]-gvecs[q];

                if (norm2(pr-sq) < 1e-12)
                {
                    if (p == r)
                    {
                        pair.d = PotVm;
                    }
                    else
                    {
                        pair.d = 1/(M_PI*L*norm2(pr));
                    }
                }
                else
                {
                    pair.d = 0;
                }
            }

            tensor.writeRemoteData({0,0,0,0}, pairs);
        }

    public:
        Jellium(const string& name, Config& config)
        : Task(name, config),
          nelec(config.get<int>("num_electrons")),
          norb(config.get<int>("num_orbitals")),
          radius(config.get<double>("radius"))
        {
            vector<Requirement> reqs;
            addProduct("scf.energy", "energy", reqs);
            addProduct("scf.E", "E", reqs);
            addProduct("scf.F", "F", reqs);
            addProduct("scf.D", "D", reqs);
            addProduct("moints", "H", reqs);

            int d = config.get<int>("dimension");
            assert(d == 3);
        }

        bool run(TaskDAG& dag, const Arena& arena)
        {
            vector<double> glen;

            for (int r = 0;;r++)
            {
                int n = 0;
                glen.clear();
                gvecs.clear();
                for (int x = -r;x <= r;x++)
                for (int y = -r;y <= r;y++)
                for (int z = -r;z <= r;z++)
                {
                    if (sqrt(x*x+y*y+z*z) < r)
                    {
                        glen.push_back(sqrt(x*x+y*y+z*z));
                        gvecs.emplace_back(x, y, z);
                        n++;
                    }
                }

                if (n >= norb) break;
            }

            cosort(glen, gvecs);

            assert(norb == gvecs.size() || std::abs(glen[norb-1] - glen[norb]) > 1e-12);
            gvecs.resize(norb);

            nocc = nelec/2;
            assert(0 < nocc && nocc <= norb);
            assert(nocc == norb || std::abs(glen[nocc-1] - glen[nocc]) > 1e-12);

            V = nelec*(4.0/3.0)*M_PI*pow(radius,3);
            L = pow(V, 1.0/3.0);
            PotVm = 2.83729747948149/L;

            vector<vector<double>> E = {vector<double>(norb)};

            int nvrt = norb-nocc;

            for (int i = 0;i < norb;i++)
            {
                E[0][i] = 2*(M_PI/L)*(M_PI/L)*norm2(gvecs[i]);
                for (int j = 0;j < nocc;j++)
                {
                    if (i == j)
                    {
                        E[0][i] -= PotVm;
                    }
                    else
                    {
                        E[0][i] -= 1/(M_PI*L*norm2(gvecs[i]-gvecs[j]));
                    }
                }
            }

            put("E", vector<vector<vector<double>>>{E,E});

            double energy = 0;
            for (int i = 0;i < nocc;i++)
            {
                energy += 2*E[0][i];
                for (int j = 0;j < nocc;j++)
                {
                    if (i == j)
                    {
                        energy += PotVm;
                    }
                    else
                    {
                        energy += 1/(M_PI*L*norm2(gvecs[i]-gvecs[j]));
                    }
                }
            }

            Tensor<SPINORBITAL|PGSYMMETRIC> F = put<Tensor<SPINORBITAL>>("F",
                Tensor<SPINORBITAL|PGSYMMETRIC>::construct("F",
                SOPGInit(PointGroup::C1(), {{norb}}, {{norb}}, {1}, {1})));
            Tensor<SPINORBITAL|PGSYMMETRIC> D = put<Tensor<SPINORBITAL>>("D", F.construct("D"));
            put("energy", energy);

            Logger::log(arena) << "SCF energy = " << setprecision(15) << energy << endl;

            KeyValueVector dpairs, fpairs;
            for (int i = 0;i < nocc;i++)
            {
                dpairs.push_back(i*norb+i, 1);
            }
            for (int i = 0;i < norb;i++)
            {
                fpairs.push_back(i*norb+i, E[0][i]);
            }

            D.setDataBySpinAndIrrep({1,1}, {0,0}, dpairs);
            D.setDataBySpinAndIrrep({0,0}, {0,0}, dpairs);
            F.setDataBySpinAndIrrep({1,1}, {0,0}, fpairs);
            F.setDataBySpinAndIrrep({0,0}, {0,0}, fpairs);

            auto& H = put("H", new TwoElectronOperator(TensorInitializer<>("H") <<
                TensorInitializer<SPINORBITAL|PGSYMMETRIC>(PointGroup::C1(), {{nvrt},{nocc}}, {{nvrt},{nocc}})));

            KeyValueVector abpairs, ijpairs;
            for (int i = 0;i < nocc;i++)
            {
                ijpairs.emplace_back(i*nocc+i, E[0][i]);
            }
            for (int i = 0;i < nvrt;i++)
            {
                abpairs.emplace_back(i*nvrt+i, E[0][i+nocc]);
            }

            H.getAB().setDataBySpinAndIrrep({1,1}, {0,0}, abpairs);
            H.getAB().setDataBySpinAndIrrep({0,0}, {0,0}, abpairs);
            H.getIJ().setDataBySpinAndIrrep({1,1}, {0,0}, ijpairs);
            H.getIJ().setDataBySpinAndIrrep({0,0}, {0,0}, ijpairs);

            /*
             * <ab||ij>
             */
            writeIntegrals(true, true, false, false, H.getABIJ()({1,0},{0,1}));
            H.getABIJ()({0,0},{0,0})["abij"] = 0.5*H.getABIJ()({1,0},{0,1})["abij"];
            H.getABIJ()({2,0},{0,2})["ABIJ"] = 0.5*H.getABIJ()({1,0},{0,1})["ABIJ"];

            /*
             * <ab||ci>
             */
            writeIntegrals(true, true, true, false, H.getABCI()({1,0},{1,0}));
            H.getABCI()({0,0},{0,0})["abci"] =  H.getABCI()({1,0},{1,0})["abci"];
            H.getABCI()({1,0},{0,1})["BacI"] = -H.getABCI()({1,0},{1,0})["aBcI"];
            H.getABCI()({2,0},{1,1})["ABCI"] =  H.getABCI()({1,0},{1,0})["ABCI"];

            /*
             * <ai||jk>
             */
            writeIntegrals(true, false, false, false, H.getAIJK()({1,0},{0,1}));
            SymmetryBlockedTensor<U> tmp(H.getAIJK()({1,0},{0,1}));
            H.getAIJK()({0,0},{0,0})["aijk"] =  H.getAIJK()({1,0},{0,1})["aijk"];
            H.getAIJK()({0,1},{0,1})["aIKj"] = -H.getAIJK()({1,0},{0,1})["aIjK"];
            H.getAIJK()({1,1},{0,2})["AIJK"] =  H.getAIJK()({1,0},{0,1})["AIJK"];

            /*
             * <ij||kl>
             */
            writeIntegrals(false, false, false, false, H.getIJKL()({0,1},{0,1}));
            H.getIJKL()({0,0},{0,0})["ijkl"] = 0.5*H.getIJKL()({0,1},{0,1})["ijkl"];
            H.getIJKL()({0,2},{0,2})["IJKL"] = 0.5*H.getIJKL()({0,1},{0,1})["IJKL"];

            /*
             * <ab||cd>
             */
            writeIntegrals(true, true, true, true, H.getABCD()({1,0},{1,0}));
            H.getABCD()({0,0},{0,0})["abcd"] = 0.5*H.getABCD()({1,0},{1,0})["abcd"];
            H.getABCD()({2,0},{2,0})["ABCD"] = 0.5*H.getABCD()({1,0},{1,0})["ABCD"];

            /*
             * <ai||bj>
             */
            SymmetryBlockedTensor<U> aijb("aijb", arena, PointGroup::C1(), 4, {{nvrt},{nocc},{nocc},{nvrt}}, {NS,NS,NS,NS}, false);
            writeIntegrals(true, false, true, false, H.getAIBJ()({0,1},{0,1}));
            H.getAIBJ()({1,0},{1,0})["AiBj"] = H.getAIBJ()({0,1},{0,1})["AiBj"];
            writeIntegrals(true, false, false, true, aijb);
            H.getAIBJ()({1,0},{0,1})["AibJ"] = -aijb["AiJb"];
            H.getAIBJ()({0,1},{1,0})["aIBj"] = -aijb["aIjB"];
            H.getAIBJ()({0,0},{0,0})["aibj"] = H.getAIBJ()({1,0},{1,0})["aibj"];
            H.getAIBJ()({1,1},{1,1})["AIBJ"] = H.getAIBJ()({1,0},{1,0})["AIBJ"];
            H.getAIBJ()({0,0},{0,0})["aibj"] -= aijb["aijb"];
            H.getAIBJ()({1,1},{1,1})["AIBJ"] -= aijb["AIJB"];

            /*
             * Fill in pieces which are equal by Hermicity
             */
            H.getIJAK()({0,2},{1,1})["JKAI"] = H.getAIJK()({1,1},{0,2})["AIJK"];
            H.getIJAK()({0,1},{1,0})["JkAi"] = H.getAIJK()({1,0},{0,1})["AiJk"];
            H.getIJAK()({0,1},{0,1})["JkaI"] = H.getAIJK()({0,1},{0,1})["aIJk"];
            H.getIJAK()({0,0},{0,0})["jkai"] = H.getAIJK()({0,0},{0,0})["aijk"];

            H.getAIBC()({1,1},{2,0})["AIBC"] = H.getABCI()({2,0},{1,1})["BCAI"];
            H.getAIBC()({1,0},{1,0})["AiBc"] = H.getABCI()({1,0},{1,0})["BcAi"];
            H.getAIBC()({0,1},{1,0})["aIBc"] = H.getABCI()({1,0},{0,1})["BcaI"];
            H.getAIBC()({0,0},{0,0})["aibc"] = H.getABCI()({0,0},{0,0})["bcai"];

            H.getIJAB()({0,2},{2,0})["IJAB"] = H.getABIJ()({2,0},{0,2})["ABIJ"];
            H.getIJAB()({0,1},{1,0})["IjAb"] = H.getABIJ()({1,0},{0,1})["AbIj"];
            H.getIJAB()({0,0},{0,0})["ijab"] = H.getABIJ()({0,0},{0,0})["abij"];

            return true;
        }
};

static const char* spec = R"!(

radius double,
num_electrons int,
num_orbitals int,
dimension? int 3

)!";

REGISTER_TASK(Jellium,"jellium",spec);

}
}

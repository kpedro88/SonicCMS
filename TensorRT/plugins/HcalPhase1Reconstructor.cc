#include <vector>
#include <map>
#include <sstream>
#include <string>
#include <cmath>
#include <utility>
#include <algorithm>

#include "Geometry/CaloGeometry/interface/CaloSubdetectorGeometry.h"
#include "Geometry/CaloGeometry/interface/CaloGeometry.h"
#include "Geometry/CaloGeometry/interface/CaloCellGeometry.h"
#include "Geometry/Records/interface/CaloGeometryRecord.h"
#include "Geometry/CaloTopology/interface/HcalTopology.h"
#include "Geometry/HcalCommonData/interface/HcalHitRelabeller.h"
#include "DataFormats/HcalRecHit/interface/HcalRecHitCollections.h"
#include "DataFormats/HcalRecHit/interface/HcalRecHitDefs.h"

#include "DataFormats/HcalDetId/interface/HcalGenericDetId.h"
#include "DataFormats/HcalDigi/interface/HcalDigiCollections.h"
#include "DataFormats/METReco/interface/HcalPhase1FlagLabels.h"

#include "CalibFormats/HcalObjects/interface/HcalDbService.h"
#include "CalibFormats/HcalObjects/interface/HcalDbRecord.h"
#include "CalibFormats/HcalObjects/interface/HcalCoderDb.h"


#include "CalibFormats/CaloObjects/interface/CaloSamples.h"
#include "CalibCalorimetry/HcalAlgos/interface/HcalSiPMnonlinearity.h"
#include "FWCore/Framework/interface/ConsumesCollector.h"

#include "SonicCMS/Core/interface/SonicEDProducer.h"
#include "SonicCMS/TensorRT/interface/TRTClient.h"
#include "FWCore/Framework/interface/Event.h"
#include "FWCore/ParameterSet/interface/ParameterSet.h"
#include "FWCore/Framework/interface/EventSetup.h"
#include "FWCore/Framework/interface/ESHandle.h"
#include "FWCore/Framework/interface/Frameworkfwd.h"
#include "FWCore/Framework/interface/MakerMacros.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"
#include "FWCore/Framework/interface/stream/EDProducer.h"
namespace {
    // Class for making SiPM/QIE11 look like HPD/QIE8. HPD/QIE8
    // needs only pedestal and gain to convert charge into energy.
    // Due to nonlinearities, response of SiPM/QIE11 is substantially
    // more complicated. It is possible to calculate all necessary
    // quantities from the charge and the info stored in the DB every
    // time the raw charge is needed. However, it does not make sense
    // to retrieve DB contents stored by channel for every time slice.
    // Because of this, we look things up only once, in the constructor.
    template<class DFrame>
    class RawChargeFromSample
    {
    public:
        inline RawChargeFromSample(const int sipmQTSShift,
                                   const int sipmQNTStoSum,
                                   const HcalDbService& cond,
                                   const HcalDetId id,
                                   const CaloSamples& cs,
                                   const int soi,
                                   const DFrame& frame,
                                   const int maxTS) {}

        inline double getRawCharge(const double decodedCharge,
                                   const double pedestal) const
            {return decodedCharge;}
    };
    template<>
    class RawChargeFromSample<QIE11DataFrame>
    {
    public:
        inline RawChargeFromSample(const int sipmQTSShift,
                                   const int sipmQNTStoSum,
                                   const HcalDbService& cond,
                                   const HcalDetId id,
                                   const CaloSamples& cs,
                                   const int soi,
                                   const QIE11DataFrame& frame,
                                   const int maxTS)
            : siPMParameter_(*cond.getHcalSiPMParameter(id)),
              fcByPE_(siPMParameter_.getFCByPE()),
              corr_(cond.getHcalSiPMCharacteristics()->getNonLinearities(siPMParameter_.getType()))
        {
            if (fcByPE_ <= 0.0)
                throw cms::Exception("HBHEPhase1BadDB")
                    << "Invalid fC/PE conversion factor for SiPM " << id
                    << std::endl;

            const HcalCalibrations& calib = cond.getHcalCalibrations(id);
            const int firstTS = std::max(soi + sipmQTSShift, 0);
            const int lastTS = std::min(firstTS + sipmQNTStoSum, maxTS);
            double sipmQ = 0.0;

            for (int ts = firstTS; ts < lastTS; ++ts)
            {
                const double pedestal = calib.pedestal(frame[ts].capid());
                sipmQ += (cs[ts] - pedestal);
            }

            const double effectivePixelsFired = sipmQ/fcByPE_;
            factor_ = corr_.getRecoCorrectionFactor(effectivePixelsFired);
        }

        inline double getRawCharge(const double decodedCharge,
                                   const double pedestal) const
        {
            return (decodedCharge - pedestal)*factor_ + pedestal;

            // Old version of TS-by-TS corrections looked as follows:
            // const double sipmQ = decodedCharge - pedestal;
            // const double nPixelsFired = sipmQ/fcByPE_;
            // return sipmQ*corr_.getRecoCorrectionFactor(nPixelsFired) + pedestal;
       }

    private:
        const HcalSiPMParameter& siPMParameter_;
        double fcByPE_;
        HcalSiPMnonlinearity corr_;
        double factor_;
    };

}

template <typename Client>
class HcalPhase1Reconstructor : public SonicEDProducer<Client>
{
	public:
		//needed because base class has dependent scope
		using typename SonicEDProducer<Client>::Input;
		using typename SonicEDProducer<Client>::Output;
		explicit HcalPhase1Reconstructor(edm::ParameterSet const& cfg) : 
			SonicEDProducer<Client>(cfg), 
			sipmQTSShift_(cfg.getParameter<unsigned>("sipmQTSShift")),
			sipmQNTStoSum_(cfg.getParameter<unsigned>("sipmQNTStoSum")), 
			topN_(cfg.getParameter<unsigned>("topN")),  
			fDigiName(cfg.getParameter<edm::InputTag>("digiLabelQIE11")),
			fRHName(cfg.getParameter<edm::InputTag>("edmRecHitName")),   
			fChanInfoName(cfg.getParameter<edm::InputTag>("edmChanInfoName")), 
			fTokRH(this->template consumes<edm::SortedCollection<HBHERecHit,edm::StrictWeakOrdering<HBHERecHit>> >(fRHName)), 
			fTokChanInfo(this->template consumes<edm::SortedCollection<HBHEChannelInfo,edm::StrictWeakOrdering<HBHEChannelInfo>> >(fChanInfoName)),
			fTokDigis(this->template consumes<QIE11DigiCollection>(fDigiName))
		{


			this->template produces<HBHERecHitCollection>();
			//for debugging
			this->setDebugName("HcalProducer");
		}
		void acquire(edm::Event const& iEvent, edm::EventSetup const& iSetup, Input& iInput) override {

			auto ninput = client_.ninput();
			auto batchSize = client_.batchSize();
			iInput = Input(ninput*batchSize, 0.f);

			edm::Handle<QIE11DigiCollection> digis;
			iEvent.getByToken(fTokDigis, digis);

			edm::ESHandle<HcalDbService> conditions;
			iSetup.get<HcalDbRecord>().get(conditions);

			tmp->clear();

		        processData<QIE11DataFrame>(*digis, *conditions, iInput, ninput);
			
		}

		template<class DFrame, class Collection>
		void processData(const Collection& coll,
                                 const HcalDbService& cond,
				 Input& iInput,
				 auto ninput)
		{

			const bool skipDroppedChannels = false;

			unsigned int ib = 0;
    			std::cout << "# digis: " << std::distance(coll.begin(), coll.end()) << std::endl;
			for (typename Collection::const_iterator it = coll.begin(); it != coll.end(); it++){



				unsigned int ib = std::distance(coll.begin(),it);

			 	const DFrame& frame(*it);
	        	  	const HcalDetId cell(frame.id());

        		   	const HcalSubdetector subdet = cell.subdet();
        			if (!(subdet == HcalSubdetector::HcalBarrel ||
	   			      subdet == HcalSubdetector::HcalEndcap ||
    			              subdet == HcalSubdetector::HcalOuter))
        		    	continue;
			
				const HcalCalibrations& calib = cond.getHcalCalibrations(cell);
			        const HcalQIECoder* channelCoder = cond.getHcalCoder(cell);
			        const HcalQIEShape* shape = cond.getHcalShape(channelCoder);
			        const HcalCoderDb coder(*channelCoder, *shape);

				CaloSamples cs;
        			coder.adc2fC(frame, cs);


				const int nRead = cs.size();
			        const int maxTS = std::min(nRead, static_cast<int>(HBHEChannelInfo::MAXSAMPLES));

				const int soi = 3;
				const int nCycles = 8;
			        const RawChargeFromSample<DFrame> rcfs(sipmQTSShift_, sipmQNTStoSum_, 
                                               			       cond, cell, cs, soi, frame, maxTS);


				
				for (int inputTS = 0; inputTS < nCycles; ++inputTS){
					auto s(frame[inputTS]);
					const uint8_t adc = s.adc();
					const int capid = s.capid();
				        const double rawCharge = rcfs.getRawCharge(cs[inputTS], calib.pedestal(capid));
					iInput[ib*ninput+inputTS+2] = (float)rawCharge;
				}

				iInput[ib + 0] = cell.ieta();
				iInput[ib + 1] = cell.iphi();
				for(unsigned int d = 1; d < 5; d++){
					if(depth == (float)d) 	{ iInput[ib*ninput + d + 10] = 1.; }
					else 			{ iInput[ib*ninput + d + 10] = 0.; }
				}
				ib++;
				HBHERecHit rh = HBHERecHit(cell, 0.f,0.f,0.f);
				tmp->push_back(rh);
			}
		}
		void produce(edm::Event& iEvent, edm::EventSetup const& iSetup, Output const& iOutput) override {


  			std::unique_ptr<HBHERecHitCollection> out;
			out = std::make_unique<HBHERecHitCollection>();
			
			unsigned int ib = 0;
			for(HBHERecHitCollection::const_iterator it = tmp->begin(); it != tmp->end(); it++){
				//std::cout << iOutput[ib] << std::endl;
				HBHERecHit rhout = HBHERecHit(it->id(),0.5,0.f,0.f);
				out->push_back(rhout);
				ib++;
			}

			iEvent.put(std::move(out));
		}
		~HcalPhase1Reconstructor() override {}

	private:

//		edm::EDGetTokenT<QIE11DigiCollection> tok_qie11_;


		int sipmQTSShift_;
		int sipmQNTStoSum_;
		unsigned topN_;
	  	edm::InputTag fDigiName;
    		edm::InputTag fRHName;
    		edm::InputTag fChanInfoName;
   		edm::EDGetTokenT<edm::SortedCollection<HBHERecHit,edm::StrictWeakOrdering<HBHERecHit>>> fTokRH;
    		edm::EDGetTokenT<edm::SortedCollection<HBHEChannelInfo,edm::StrictWeakOrdering<HBHEChannelInfo>>> fTokChanInfo;
 		edm::EDGetTokenT<QIE11DigiCollection> fTokDigis;

                std::vector<HBHERecHit> tmprh;
		std::vector<HBHERecHit> *tmp = &tmprh;
		
		float depth, ieta, iphi; 

		using SonicEDProducer<Client>::client_;
		//Just putting something in for the hell of it

};


typedef HcalPhase1Reconstructor<TRTClientSync> HcalPhase1ReconstructorSync;
typedef HcalPhase1Reconstructor<TRTClientAsync> HcalPhase1ReconstructorAsync;
typedef HcalPhase1Reconstructor<TRTClientPseudoAsync> HcalPhase1ReconstructorPseudoAsync;

DEFINE_FWK_MODULE(HcalPhase1ReconstructorSync);
DEFINE_FWK_MODULE(HcalPhase1ReconstructorAsync);
DEFINE_FWK_MODULE(HcalPhase1ReconstructorPseudoAsync);

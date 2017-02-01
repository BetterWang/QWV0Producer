#include "QWV0Fitter.h"

#include "TrackingTools/TransientTrack/interface/TransientTrackBuilder.h"
#include "TrackingTools/Records/interface/TransientTrackRecord.h"
#include "TrackingTools/PatternTools/interface/ClosestApproachInRPhi.h"
#include "Geometry/CommonDetUnit/interface/GlobalTrackingGeometry.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"
#include "TrackingTools/TrajectoryState/interface/TrajectoryStateTransform.h"
#include "TrackingTools/PatternTools/interface/TSCBLBuilderNoMaterial.h"
#include <Math/Functions.h>
#include <Math/SVector.h>
#include <Math/SMatrix.h>
#include <typeinfo>
#include <memory>
#include "DataFormats/VertexReco/interface/Vertex.h"
#include "CommonTools/CandUtils/interface/AddFourMomenta.h"

// pdg mass constants
namespace {
	const double piMass = 0.13957018;
	const double piMassSquared = piMass*piMass;
	const double protonMass = 0.938272046;
	const double protonMassSquared = protonMass*protonMass;
	const double kaonMass = 0.493667;
	const double kaonMassSquared = kaonMass*kaonMass;
	const double kShortMass = 0.497614;
	const double lambdaMass = 1.115683;
	const double D0Mass = 1.86484;
}

typedef ROOT::Math::SMatrix<double, 3, 3, ROOT::Math::MatRepSym<double, 3> > SMatrixSym3D;
typedef ROOT::Math::SVector<double, 3> SVector3;

QWV0Fitter::QWV0Fitter(const edm::ParameterSet& theParameters, edm::ConsumesCollector && iC)
{
	token_beamSpot = iC.consumes<reco::BeamSpot>(theParameters.getParameter<edm::InputTag>("beamSpot"));
	useVertex_ = theParameters.getParameter<bool>("useVertex");
	token_vertices = iC.consumes<std::vector<reco::Vertex>>(theParameters.getParameter<edm::InputTag>("vertices"));

	token_tracks = iC.consumes<reco::TrackCollection>(theParameters.getParameter<edm::InputTag>("trackRecoAlgorithm"));
	vertexFitter_ = theParameters.getParameter<bool>("vertexFitter");
	useRefTracks_ = theParameters.getParameter<bool>("useRefTracks");

	// whether to reconstruct KShorts
	doKShorts_ = theParameters.getParameter<bool>("doKShorts");
	// whether to reconstruct Lambdas
	doLambdas_ = theParameters.getParameter<bool>("doLambdas");
	// whether to reconstruct D0s
	doD0s_ = theParameters.getParameter<bool>("doD0s");

	// cuts on initial track selection
	tkChi2Cut_ = theParameters.getParameter<double>("tkChi2Cut");
	tkNHitsCut_ = theParameters.getParameter<int>("tkNHitsCut");
	tkPtCut_ = theParameters.getParameter<double>("tkPtCut");
	tkIPSigXYCut_ = theParameters.getParameter<double>("tkIPSigXYCut");
	tkIPSigZCut_ = theParameters.getParameter<double>("tkIPSigZCut");

	// cuts on vertex
	vtxChi2Cut_ = theParameters.getParameter<double>("vtxChi2Cut");
	vtxDecaySigXYZCut_ = theParameters.getParameter<double>("vtxDecaySigXYZCut");
	vtxDecaySigXYCut_ = theParameters.getParameter<double>("vtxDecaySigXYCut");
	// miscellaneous cuts
	tkDCACut_ = theParameters.getParameter<double>("tkDCACut");
	mPiPiCut_ = theParameters.getParameter<double>("mPiPiCut");
	innerHitPosCut_ = theParameters.getParameter<double>("innerHitPosCut");
	cosThetaXYCut_ = theParameters.getParameter<double>("cosThetaXYCut");
	cosThetaXYZCut_ = theParameters.getParameter<double>("cosThetaXYZCut");
	// cuts on the V0 candidate mass
	kShortMassCut_ = theParameters.getParameter<double>("kShortMassCut");
	lambdaMassCut_ = theParameters.getParameter<double>("lambdaMassCut");
	D0MassCut_ = theParameters.getParameter<double>("D0MassCut");
}

// method containing the algorithm for vertex reconstruction
void QWV0Fitter::fitAll(const edm::Event& iEvent, const edm::EventSetup& iSetup,
		reco::VertexCompositeCandidateCollection & theKshorts,
		reco::VertexCompositeCandidateCollection & theLambdas,
		reco::VertexCompositeCandidateCollection & theD0s)
{
	using std::vector;

	edm::Handle<reco::TrackCollection> theTrackHandle;
	iEvent.getByToken(token_tracks, theTrackHandle);
	if (!theTrackHandle->size()) return;
	const reco::TrackCollection* theTrackCollection = theTrackHandle.product();

	edm::Handle<reco::BeamSpot> theBeamSpotHandle;
	iEvent.getByToken(token_beamSpot, theBeamSpotHandle);
	const reco::BeamSpot* theBeamSpot = theBeamSpotHandle.product();
	math::XYZPoint referencePos(theBeamSpot->position());

	reco::Vertex referenceVtx;
	if (useVertex_) {
		edm::Handle<std::vector<reco::Vertex>> vertices;
		iEvent.getByToken(token_vertices, vertices);
		referenceVtx = vertices->at(0);
		referencePos = referenceVtx.position();
	}

	edm::ESHandle<MagneticField> theMagneticFieldHandle;
	iSetup.get<IdealMagneticFieldRecord>().get(theMagneticFieldHandle);
	const MagneticField* theMagneticField = theMagneticFieldHandle.product();

	std::vector<reco::TrackRef> theTrackRefs;
	std::vector<reco::TransientTrack> theTransTracks;

	// fill vectors of TransientTracks and TrackRefs after applying preselection cuts
	for (reco::TrackCollection::const_iterator iTk = theTrackCollection->begin(); iTk != theTrackCollection->end(); ++iTk) {
		const reco::Track* tmpTrack = &(*iTk);
		double ipsigXY = std::abs(tmpTrack->dxy(*theBeamSpot)/tmpTrack->dxyError());
		if (useVertex_) ipsigXY = std::abs(tmpTrack->dxy(referencePos)/tmpTrack->dxyError());
		double ipsigZ = std::abs(tmpTrack->dz(referencePos)/tmpTrack->dzError());
		if (tmpTrack->normalizedChi2() < tkChi2Cut_ && tmpTrack->numberOfValidHits() >= tkNHitsCut_ &&
				tmpTrack->pt() > tkPtCut_ && ipsigXY > tkIPSigXYCut_ && ipsigZ > tkIPSigZCut_) {
			reco::TrackRef tmpRef(theTrackHandle, std::distance(theTrackCollection->begin(), iTk));
			theTrackRefs.push_back(std::move(tmpRef));
			reco::TransientTrack tmpTransient(*tmpRef, theMagneticField);
			theTransTracks.push_back(std::move(tmpTransient));
		}
	}
	// good tracks have now been selected for vertexing

	// loop over tracks and vertex good charged track pairs
	for (unsigned int trdx1 = 0; trdx1 < theTrackRefs.size(); ++trdx1) {
		for (unsigned int trdx2 = trdx1 + 1; trdx2 < theTrackRefs.size(); ++trdx2) {

			reco::TrackRef positiveTrackRef;
			reco::TrackRef negativeTrackRef;
			reco::TransientTrack* posTransTkPtr = nullptr;
			reco::TransientTrack* negTransTkPtr = nullptr;

			if (theTrackRefs[trdx1]->charge() < 0. && theTrackRefs[trdx2]->charge() > 0.) {
				negativeTrackRef = theTrackRefs[trdx1];
				positiveTrackRef = theTrackRefs[trdx2];
				negTransTkPtr = &theTransTracks[trdx1];
				posTransTkPtr = &theTransTracks[trdx2];
			} else if (theTrackRefs[trdx1]->charge() > 0. && theTrackRefs[trdx2]->charge() < 0.) {
				negativeTrackRef = theTrackRefs[trdx2];
				positiveTrackRef = theTrackRefs[trdx1];
				negTransTkPtr = &theTransTracks[trdx2];
				posTransTkPtr = &theTransTracks[trdx1];
			} else {
//				std::cout << " --> " << __LINE__ << std::endl;
				continue;
			}

			// measure distance between tracks at their closest approach
			if (!posTransTkPtr->impactPointTSCP().isValid() || !negTransTkPtr->impactPointTSCP().isValid()) {
				std::cout << " --> " << __LINE__ << std::endl;
				continue;
			}
			FreeTrajectoryState const & posState = posTransTkPtr->impactPointTSCP().theState();
			FreeTrajectoryState const & negState = negTransTkPtr->impactPointTSCP().theState();
			ClosestApproachInRPhi cApp;
			cApp.calculate(posState, negState);
			if (!cApp.status()) {
				std::cout << " --> " << __LINE__ << " cApp.status() = " << cApp.status() << std::endl;
				continue;
			}
			float dca = std::abs(cApp.distance());
			if (dca > tkDCACut_) {
				std::cout << " --> " << __LINE__ << " dca = " << dca << std::endl;
				continue;
			}

			// the POCA should at least be in the sensitive volume
			GlobalPoint cxPt = cApp.crossingPoint();
			if (sqrt(cxPt.x()*cxPt.x() + cxPt.y()*cxPt.y()) > 120. || std::abs(cxPt.z()) > 300.) {
				std::cout << " --> " << __LINE__ << std::endl;
				continue;
			}

			// the tracks should at least point in the same quadrant
			TrajectoryStateClosestToPoint const & posTSCP = posTransTkPtr->trajectoryStateClosestToPoint(cxPt);
			TrajectoryStateClosestToPoint const & negTSCP = negTransTkPtr->trajectoryStateClosestToPoint(cxPt);
			if (!posTSCP.isValid() || !negTSCP.isValid()) {
				std::cout << " --> " << __LINE__ << std::endl;
				continue;
			}
			if (posTSCP.momentum().dot(negTSCP.momentum())  < 0) {
//				std::cout << " --> " << __LINE__ << std::endl;
//				continue;
			}

			// calculate mPiPi
			double totalE = sqrt(posTSCP.momentum().mag2() + piMassSquared) + sqrt(negTSCP.momentum().mag2() + piMassSquared);
			double totalESq = totalE*totalE;
			double totalPSq = (posTSCP.momentum() + negTSCP.momentum()).mag2();
			double mass = sqrt(totalESq - totalPSq);
			if (mass > mPiPiCut_) {
				std::cout << " --> " << __LINE__ << " mPiPi = " << mass << std::endl;
				continue;
			}

			// Fill the vector of TransientTracks to send to KVF
			std::vector<reco::TransientTrack> transTracks;
			transTracks.reserve(2);
			transTracks.push_back(*posTransTkPtr);
			transTracks.push_back(*negTransTkPtr);

			// create the vertex fitter object and vertex the tracks
			TransientVertex theRecoVertex;
			if (vertexFitter_) {
				KalmanVertexFitter theKalmanFitter(useRefTracks_ == 0 ? false : true);
				theRecoVertex = theKalmanFitter.vertex(transTracks);
			} else if (!vertexFitter_) {
				useRefTracks_ = false;
				AdaptiveVertexFitter theAdaptiveFitter;
				theRecoVertex = theAdaptiveFitter.vertex(transTracks);
			}
			if (!theRecoVertex.isValid()) {
//				std::cout << " --> " << __LINE__ << std::endl;
				continue;
			}

			reco::Vertex theVtx = theRecoVertex;
			if (theVtx.normalizedChi2() > vtxChi2Cut_) {
				std::cout << " --> " << __LINE__ << " theVtx.normalizedChi2() " << theVtx.normalizedChi2() << std::endl;
				continue;
			}
			GlobalPoint vtxPos(theVtx.x(), theVtx.y(), theVtx.z());

			// 2D decay significance
			SMatrixSym3D totalCov = theBeamSpot->rotatedCovariance3D() + theVtx.covariance();
			if (useVertex_) totalCov = referenceVtx.covariance() + theVtx.covariance();
			SVector3 distVecXY(vtxPos.x()-referencePos.x(), vtxPos.y()-referencePos.y(), 0.);
			double distMagXY = ROOT::Math::Mag(distVecXY);
			double sigmaDistMagXY = sqrt(ROOT::Math::Similarity(totalCov, distVecXY)) / distMagXY;
			if (distMagXY/sigmaDistMagXY < vtxDecaySigXYCut_) {
				std::cout << " --> " << __LINE__ << " distMagXY/sigmaDistMagXY = " << distMagXY/sigmaDistMagXY << std::endl;
				continue;
			}

			// 3D decay significance
			SVector3 distVecXYZ(vtxPos.x()-referencePos.x(), vtxPos.y()-referencePos.y(), vtxPos.z()-referencePos.z());
			double distMagXYZ = ROOT::Math::Mag(distVecXYZ);
			double sigmaDistMagXYZ = sqrt(ROOT::Math::Similarity(totalCov, distVecXYZ)) / distMagXYZ;
			if (distMagXYZ/sigmaDistMagXYZ < vtxDecaySigXYZCut_) {
				std::cout << " --> " << __LINE__ << " distMagXYZ/sigmaDistMagXYZ = " << distMagXYZ/sigmaDistMagXYZ << std::endl;
				continue;
			}

		// make sure the vertex radius is within the inner track hit radius
		// comment out for TrackExtra
//			if (innerHitPosCut_ > 0. && positiveTrackRef->innerOk()) {
//				reco::Vertex::Point posTkHitPos = positiveTrackRef->innerPosition();
//				double posTkHitPosD2 =  (posTkHitPos.x()-referencePos.x())*(posTkHitPos.x()-referencePos.x()) +
//					(posTkHitPos.y()-referencePos.y())*(posTkHitPos.y()-referencePos.y());
//				if (sqrt(posTkHitPosD2) < (distMagXY - sigmaDistMagXY*innerHitPosCut_)) continue;
//			}
//			if (innerHitPosCut_ > 0. && negativeTrackRef->innerOk()) {
//				reco::Vertex::Point negTkHitPos = negativeTrackRef->innerPosition();
//				double negTkHitPosD2 = (negTkHitPos.x()-referencePos.x())*(negTkHitPos.x()-referencePos.x()) +
//					(negTkHitPos.y()-referencePos.y())*(negTkHitPos.y()-referencePos.y());
//				if (sqrt(negTkHitPosD2) < (distMagXY - sigmaDistMagXY*innerHitPosCut_)) continue;
//			}

			std::auto_ptr<TrajectoryStateClosestToPoint> trajPlus;
			std::auto_ptr<TrajectoryStateClosestToPoint> trajMins;
			std::vector<reco::TransientTrack> theRefTracks;
			if (theRecoVertex.hasRefittedTracks()) {
				theRefTracks = theRecoVertex.refittedTracks();
			}

			if (useRefTracks_ && theRefTracks.size() > 1) {
				reco::TransientTrack* thePositiveRefTrack = 0;
				reco::TransientTrack* theNegativeRefTrack = 0;
				for (std::vector<reco::TransientTrack>::iterator iTrack = theRefTracks.begin(); iTrack != theRefTracks.end(); ++iTrack) {
					if (iTrack->track().charge() > 0.) {
						thePositiveRefTrack = &*iTrack;
					} else if (iTrack->track().charge() < 0.) {
						theNegativeRefTrack = &*iTrack;
					}
				}
				if (thePositiveRefTrack == 0 || theNegativeRefTrack == 0) continue;
				trajPlus.reset(new TrajectoryStateClosestToPoint(thePositiveRefTrack->trajectoryStateClosestToPoint(vtxPos)));
				trajMins.reset(new TrajectoryStateClosestToPoint(theNegativeRefTrack->trajectoryStateClosestToPoint(vtxPos)));
			} else {
				trajPlus.reset(new TrajectoryStateClosestToPoint(posTransTkPtr->trajectoryStateClosestToPoint(vtxPos)));
				trajMins.reset(new TrajectoryStateClosestToPoint(negTransTkPtr->trajectoryStateClosestToPoint(vtxPos)));
			}

			if (trajPlus.get() == 0 || trajMins.get() == 0 || !trajPlus->isValid() || !trajMins->isValid()) continue;

			GlobalVector positiveP(trajPlus->momentum());
			GlobalVector negativeP(trajMins->momentum());
			GlobalVector totalP(positiveP + negativeP);

			// 2D pointing angle
			double dx = theVtx.x()-referencePos.x();
			double dy = theVtx.y()-referencePos.y();
			double px = totalP.x();
			double py = totalP.y();
			double angleXY = (dx*px+dy*py)/(sqrt(dx*dx+dy*dy)*sqrt(px*px+py*py));
			if (angleXY < cosThetaXYCut_) {
				std::cout << " --> " << __LINE__ << " angleXY = " << angleXY << std::endl;
				continue;
			}

			// 3D pointing angle
			double dz = theVtx.z()-referencePos.z();
			double pz = totalP.z();
			double angleXYZ = (dx*px+dy*py+dz*pz)/(sqrt(dx*dx+dy*dy+dz*dz)*sqrt(px*px+py*py+pz*pz));
			if (angleXYZ < cosThetaXYZCut_) {
				std::cout << " --> " << __LINE__ << " angleXYZ = " << angleXYZ << std::endl;
				continue;
			}

			// calculate total energy of V0 4 ways: assume it's a kShort, a Lambda, or a LambdaBar, D0.
			double piPlusE = sqrt(positiveP.mag2() + piMassSquared);
			double piMinusE = sqrt(negativeP.mag2() + piMassSquared);
			double kaonPlusE = sqrt(positiveP.mag2() + kaonMassSquared);
			double kaonMinusE = sqrt(negativeP.mag2() + kaonMassSquared);
			double protonE = sqrt(positiveP.mag2() + protonMassSquared);
			double antiProtonE = sqrt(negativeP.mag2() + protonMassSquared);
			double kShortETot = piPlusE + piMinusE;
			double lambdaEtot = protonE + piMinusE;
			double lambdaBarEtot = antiProtonE + piPlusE;
			double D0Etot = kaonMinusE + piPlusE;
			double D0BarEtot = kaonPlusE + piMinusE;

			// Create momentum 4-vectors for the 3 candidate types
			const reco::Particle::LorentzVector kShortP4(totalP.x(), totalP.y(), totalP.z(), kShortETot);
			const reco::Particle::LorentzVector lambdaP4(totalP.x(), totalP.y(), totalP.z(), lambdaEtot);
			const reco::Particle::LorentzVector lambdaBarP4(totalP.x(), totalP.y(), totalP.z(), lambdaBarEtot);
			const reco::Particle::LorentzVector D0P4(totalP.x(), totalP.y(), totalP.z(), D0Etot);
			const reco::Particle::LorentzVector D0BarP4(totalP.x(), totalP.y(), totalP.z(), D0BarEtot);

			reco::Particle::Point vtx(theVtx.x(), theVtx.y(), theVtx.z());
			const reco::Vertex::CovarianceMatrix vtxCov(theVtx.covariance());
			double vtxChi2(theVtx.chi2());
			double vtxNdof(theVtx.ndof());

			// Create the VertexCompositeCandidate object that will be stored in the Event
			reco::VertexCompositeCandidate* theKshort = nullptr;
			reco::VertexCompositeCandidate* theLambda = nullptr;
			reco::VertexCompositeCandidate* theLambdaBar = nullptr;
			reco::VertexCompositeCandidate* theD0 = nullptr;
			reco::VertexCompositeCandidate* theD0Bar = nullptr;

			if (doKShorts_) {
				theKshort = new reco::VertexCompositeCandidate(0, kShortP4, vtx, vtxCov, vtxChi2, vtxNdof);
			}
			if (doLambdas_) {
				if (positiveP.mag2() > negativeP.mag2()) {
					theLambda = new reco::VertexCompositeCandidate(0, lambdaP4, vtx, vtxCov, vtxChi2, vtxNdof);
				} else {
					theLambdaBar = new reco::VertexCompositeCandidate(0, lambdaBarP4, vtx, vtxCov, vtxChi2, vtxNdof);
				}
			}
			if (doD0s_) {
					theD0 = new reco::VertexCompositeCandidate(0, D0P4, vtx, vtxCov, vtxChi2, vtxNdof);
					theD0Bar = new reco::VertexCompositeCandidate(0, D0BarP4, vtx, vtxCov, vtxChi2, vtxNdof);
			}

			// Create daughter candidates for the VertexCompositeCandidates
			reco::RecoChargedCandidate thePiPlusCand(
					1, reco::Particle::LorentzVector(positiveP.x(), positiveP.y(), positiveP.z(), piPlusE), vtx);
			thePiPlusCand.setTrack(positiveTrackRef);

			reco::RecoChargedCandidate thePiMinusCand(
					-1, reco::Particle::LorentzVector(negativeP.x(), negativeP.y(), negativeP.z(), piMinusE), vtx);
			thePiMinusCand.setTrack(negativeTrackRef);

			reco::RecoChargedCandidate theProtonCand(
					1, reco::Particle::LorentzVector(positiveP.x(), positiveP.y(), positiveP.z(), protonE), vtx);
			theProtonCand.setTrack(positiveTrackRef);

			reco::RecoChargedCandidate theAntiProtonCand(
					-1, reco::Particle::LorentzVector(negativeP.x(), negativeP.y(), negativeP.z(), antiProtonE), vtx);
			theAntiProtonCand.setTrack(negativeTrackRef);

			reco::RecoChargedCandidate theKaonPlusCand(
					1, reco::Particle::LorentzVector(positiveP.x(), positiveP.y(), positiveP.z(), kaonPlusE), vtx);
			theKaonPlusCand.setTrack(positiveTrackRef);

			reco::RecoChargedCandidate theKaonMinusCand(
					-1, reco::Particle::LorentzVector(negativeP.x(), negativeP.y(), negativeP.z(), kaonMinusE), vtx);
			theKaonMinusCand.setTrack(negativeTrackRef);

			AddFourMomenta addp4;
			// Store the daughter Candidates in the VertexCompositeCandidates if they pass mass cuts
			if (doKShorts_) {
				theKshort->addDaughter(thePiPlusCand);
				theKshort->addDaughter(thePiMinusCand);
				theKshort->setPdgId(310);
				addp4.set(*theKshort);
				if (theKshort->mass() < kShortMass + kShortMassCut_ && theKshort->mass() > kShortMass - kShortMassCut_) {
					theKshorts.push_back(std::move(*theKshort));
				}
			}
			if (doLambdas_ && theLambda) {
				theLambda->addDaughter(theProtonCand);
				theLambda->addDaughter(thePiMinusCand);
				theLambda->setPdgId(3122);
				addp4.set( *theLambda );
				if (theLambda->mass() < lambdaMass + lambdaMassCut_ && theLambda->mass() > lambdaMass - lambdaMassCut_) {
					theLambdas.push_back(std::move(*theLambda));
				}
			} else if (doLambdas_ && theLambdaBar) {
				theLambdaBar->addDaughter(theAntiProtonCand);
				theLambdaBar->addDaughter(thePiPlusCand);
				theLambdaBar->setPdgId(-3122);
				addp4.set(*theLambdaBar);
				if (theLambdaBar->mass() < lambdaMass + lambdaMassCut_ && theLambdaBar->mass() > lambdaMass - lambdaMassCut_) {
					theLambdas.push_back(std::move(*theLambdaBar));
				}
			}

			if (doD0s_ ) {
				theD0->addDaughter(thePiPlusCand);
				theD0->addDaughter(theKaonMinusCand);
				theD0->setPdgId(421);
				addp4.set(*theD0);
				std::cout << " --> " << __LINE__ << " theD0->mass() = " << theD0->mass() << std::endl;
				if ( theD0->mass() < D0Mass + D0MassCut_ and theD0->mass() > D0Mass - D0MassCut_ ) {
					theD0s.push_back(std::move(*theD0));
				}
				theD0Bar->addDaughter(theKaonPlusCand);
				theD0Bar->addDaughter(thePiMinusCand);
				theD0Bar->setPdgId(-421);
				addp4.set(*theD0Bar);
				std::cout << " --> " << __LINE__ << " theD0Bar->mass() = " << theD0Bar->mass() << std::endl;
				if ( theD0Bar->mass() < D0Mass + D0MassCut_ and theD0Bar->mass() > D0Mass - D0MassCut_ ) {
					theD0s.push_back(std::move(*theD0Bar));
				}
			}

			delete theKshort;
			delete theLambda;
			delete theLambdaBar;
			delete theD0;
			delete theD0Bar;
			theKshort = theLambda = theLambdaBar = theD0 = theD0Bar = nullptr;
		}
	}
}


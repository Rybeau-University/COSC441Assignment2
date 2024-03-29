/*
 * CsmaMac.cc
 *  Authors: Juliet Samandari & Ryan Beaumont
 *   Date: May 2021
 */

#include "CsmaMac.h"
#include "AppMessage_m.h"
#include "AppResponse_m.h"
#include "CSRequest_m.h"
#include "CSResponse_m.h"
#include "MacPacket_m.h"
#include "MacPacketType_m.h"
#include "TransmissionRequest_m.h"
#include "TransmissionConfirmation_m.h"
#include "TransmissionIndication_m.h"
#include <queue>


Define_Module(CsmaMac);

using namespace omnetpp;

simsignal_t bufferLossSigId     = cComponent::registerSignal("bufferLossSig");
simsignal_t bufferEnteredSigId  = cComponent::registerSignal("bufferEnteredSig");
simsignal_t numberAttemptsSigId = cComponent::registerSignal("numberAttemptsSig");
simsignal_t accessFailedSigId   = cComponent::registerSignal("accessFailedSig");
simsignal_t accessSuccessSigId  = cComponent::registerSignal("accessSuccessSig");

void CsmaMac::initialize () {
    ownAddress          = par("ownAddress");
    bufferSize          = par("bufferSize");
    maxBackoffs         = par("maxBackoffs");
    maxAttempts         = par("maxAttempts");
    macOverheadSizeData = par("macOverheadSizeData");
    macOverheadSizeAck  = par("macOverheadSizeAck");
    macAckDelay         = par("macAckDelay");
    ackTimeout          = par("ackTimeout");
    fromHigherId        = findGate("fromHigher");
    toHigherId          = findGate("toHigher");
    fromTransceiverId   = findGate("fromTransceiver");
    toTransceiverId     = findGate("toTransceiver");
    backOffComplete = new cMessage ("BackOffComplete");
    ackTimeoutMessage = new cMessage ("AckTimeout");
    ackSendMessage = new cMessage ("AckSendMessage");


    assert(bufferSize > 0);
    assert(maxBackoffs > 0);
    assert(maxAttempts > 0);
    assert(macOverheadSizeData > 0);
    assert(macOverheadSizeAck > 0);
    assert(macAckDelay >= 0);
    assert(ackTimeout > 0);
}

/**
 * Handles incoming messages on the gates.
 */
void CsmaMac::handleMessage(cMessage* msg){
    dbg_string("----------------------------------------------");
    dbg_enter("handleMessage");
    int arrivalGate = msg->getArrivalGateId();

    if (dynamic_cast<AppMessage*>(msg) && arrivalGate == fromHigherId){
           receiveAppMessage((AppMessage*) msg);
           return;
    }

    if (dynamic_cast<CSResponse*>(msg) && arrivalGate == fromTransceiverId && currentState == STATE_CS){
        dbg_string("Received CSReponse Message");
        handleCSResponse((CSResponse*) msg);
        dbg_leave("handleMessage");
        return;
    }



    if(dynamic_cast<TransmissionConfirmation*>(msg) && arrivalGate == fromTransceiverId && currentState == STATE_TCONF){
        dbg_string("Transmission Confirmation Received");
        handleTransmissionConfirmation((TransmissionConfirmation*) msg);
        dbg_leave("handleMessage");
        return;
    }

    if(dynamic_cast<TransmissionConfirmation*>(msg) && arrivalGate == fromTransceiverId){
        dbg_string("Transmission Confirmation for Ack Received");
        delete msg;
        dbg_leave("handleMessage");
        return;
    }

    if(dynamic_cast<TransmissionIndication*>(msg) && arrivalGate == fromTransceiverId){
        dbg_string("Transmission Indication Received");
        handleTransmissionIndication((TransmissionIndication*) msg);
        dbg_leave("handleMessage");
        return;
    }

    if(msg == backOffComplete && currentState == STATE_BACKOFF){ //Need to check message on buffer
        dbg_string("Backoff Complete");
        checkBuffer();
        dbg_leave("handleMessage");
        return;
    }


    if(msg == ackTimeoutMessage && currentState == STATE_ACK){
        dbg_string("Ack Timeout Message Received");
        handleAckTimeout();
        dbg_leave("handleMessage");
        return;
    }
    if (msg == ackSendMessage && msg->isSelfMessage()){
        dbg_string("Ack Completed Message Received");
        transmitAckForReceived((AppMessage*) ackQueue.front());
        delete ackQueue.front();
        ackQueue.pop();
        dbg_leave("handleMessage");
        return;
    }

    delete msg;
    error("CsmaMac::handleMessage: unexpected message");
}

/**
 * Handles dropping packets that have not been successfully sent due to the maximum carrier sense backoff attempts occurring.
 * Should be called if transmission has not even been attempted as the channel was always busy.
 */
void CsmaMac::dropPacketChannelFail(void){
    dbg_enter("dropPacketCS");
    AppMessage* appMsg = buffer.front();
    buffer.pop();
    AppResponse* aResponse = new AppResponse;
    aResponse->setSequenceNumber(appMsg->getSequenceNumber());
    aResponse->setOutcome(ChannelFailure);
    send(aResponse, toHigherId);
    delete appMsg;
    emit(accessFailedSigId, true);
    dbg_leave("dropPacketCS");
}

/**
 * Drops packet in successful reception of packet by receiver.
 */
void CsmaMac::dropPacketSuccess(void){
    dbg_enter("dropPacketSuccess");
    AppMessage* appMsg = buffer.front();
    buffer.pop();
    AppResponse* aResponse = new AppResponse;
    aResponse->setSequenceNumber(appMsg->getSequenceNumber());
    aResponse->setOutcome(Success);
    send(aResponse, toHigherId);
    delete appMsg;
    emit(accessSuccessSigId, true);
    dbg_leave("dropPacketSuccess");
}

/**
 * Handles AppMessages that are to be dropped due to a full buffer.
 */
void CsmaMac::dropAppMessage(AppMessage* appMsg){
    dbg_enter("dropAppMessage");

    AppResponse* aResponse = new AppResponse;
    aResponse->setSequenceNumber(appMsg->getSequenceNumber());
    aResponse->setOutcome(BufferDrop);
    send(aResponse, toHigherId);
    delete appMsg;
    emit(bufferLossSigId, true);
    dbg_leave("dropAppMessage");
}

/**
 * Handles AppMessages that are received from the higher level.
 */
void CsmaMac::receiveAppMessage(AppMessage* appMsg){
    dbg_enter("receiveAppMessage");
    if (buffer.size() < bufferSize) {
        buffer.push(appMsg);
        emit(bufferEnteredSigId, true);
    } else {
        dropAppMessage(appMsg);
    }
    if (currentState == STATE_IDLE) {
        checkBuffer();
    }
    dbg_leave("receiveAppMessage");
}

/**
 * Checks buffer for messages to transmit.
 */
void CsmaMac::checkBuffer(){
    dbg_enter("checkBuffer");
    if (buffer.empty()) {
        currentState = STATE_IDLE;
    } else {
        if (currentAttempts < maxAttempts){
            performCarrierSense();
        } else {
            dbg_string("Max Attempts Reached");
            dropPacketChannelFail();
            emit(numberAttemptsSigId, currentAttempts);
            currentAttempts = 0;
            currentBackoffs = 0;
            checkBuffer();
        }
    }

    dbg_leave("checkBuffer");
}

/**
 * Sends the CSRequest message to the transceiver.
 */
void CsmaMac::performCarrierSense(){
    dbg_enter("performCarrierSense");
    CSRequest* request = new CSRequest;
    dbg_string("Sending CSRequest");
    send(request, toTransceiverId);
    currentState = STATE_CS;
    dbg_leave("performCarrierSense");
}

/**
 * Handles the received CSResponse message from the transceiver.
 */
void CsmaMac::handleCSResponse(CSResponse* response){
    dbg_enter("handleCSResponse");
    if (!response->getBusyChannel()){
        transmitHOLPacket();
    } else {
        if (currentBackoffs < maxBackoffs){
            currentBackoffs++;
            dbg_string("Beginning CSResponse Backoff");
            beginBackoff(par("csBackoffDistribution").doubleValue());
        } else {
            dbg_string("Max backoffs reached");
            currentBackoffs = 0;
            currentAttempts++;
            dbg_string("Beginning Attempts Backoff");
            beginBackoff(par("attBackoffDistribution").doubleValue());
        }
    }
    delete response;
    dbg_leave("handleCSReponse");
}

/**
 * Handles Reception of AckTimeout Message
 */
void CsmaMac::handleAckTimeout(){
    dbg_enter("handAckTimeout");
    dbg_string("Entering Failure Backoff");
    beginBackoff(par("attBackoffDistribution").doubleValue());
    currentBackoffs = 0;
    currentAttempts++;
    dbg_leave("handAckTimeout");
}

/**
 * Handles incoming TransmissionConfirmation messages and start the ack timeout.
 */
void CsmaMac::handleTransmissionConfirmation(TransmissionConfirmation* confirmation){
    dbg_enter("HandleTransmissionConfirmation");
    dbg_string("Starting Ack Timeout");
    scheduleAt(simTime() + ackTimeout, ackTimeoutMessage);
    currentState = STATE_ACK;
    delete confirmation;
    dbg_leave("HandleTransmissionConfirmation");
}

/**
 * Handles incoming TransmissionIndication messages
 */
void CsmaMac::handleTransmissionIndication(TransmissionIndication* indication){
    dbg_enter("handleTransmissionIndication");
    MacPacket* macPacket = (MacPacket*) indication->decapsulate();
    if (macPacket->getReceiverAddress() == ownAddress){
        if (macPacket->getMacPacketType() == MacAckPacket){
            handleAck(macPacket);
        } else {
            handleReceivedMessage(macPacket);
        }
    } else {
        error("CsmaMac::handleTransmissionIndication: Message with wrong address");
    }
    delete macPacket;
    delete indication;
    dbg_leave("handleTransmissionIndication");
}

/**
 * Handles a received message.
 */
void CsmaMac::handleReceivedMessage(MacPacket* macPacket) {
    dbg_enter("handleReceivedMessage");
    AppMessage* appMsg = (AppMessage*) macPacket->decapsulate();
    ackQueue.push(appMsg->dup());
    send(appMsg, toHigherId);
    scheduleAt(simTime() + macAckDelay, ackSendMessage);
    dbg_leave("handleReceivedMessage");
}

/**
 * Transmits Ack for received packet.
 */
void CsmaMac::transmitAckForReceived(AppMessage* appMsg) {
    dbg_enter("transmitAckForReceived");
    MacPacket* macPacket = new MacPacket;
    macPacket->setReceiverAddress(appMsg->getSenderAddress());
    macPacket->setTransmitterAddress(ownAddress);
    macPacket->setMacPacketType(MacAckPacket);
    macPacket->setByteLength(macOverheadSizeAck);
    TransmissionRequest* tRequest = encapsulateMacPacket(macPacket);
    send(tRequest, toTransceiverId);
    dbg_leave("transmitAckForReceived");
}

/**
 * Handles incoming acks.
 */
void CsmaMac::handleAck(MacPacket* macPacket){
    dbg_enter("handleAck");
    if (currentState == STATE_ACK && macPacket->getTransmitterAddress() == buffer.front()->getReceiverAddress()){
        cancelEvent(ackTimeoutMessage);
        emit(numberAttemptsSigId, currentAttempts);
        currentAttempts = 0;
        currentBackoffs = 0;
        dropPacketSuccess();
        beginBackoff(par("succBackoffDistribution").doubleValue());
    } else if (macPacket->getTransmitterAddress() != buffer.front()->getReceiverAddress()){
        dbg_string("Ack received not for HOL packet.");
    } else{
        dbg_string("Ack received after timeout.");
    }
    dbg_leave("handleAck");
}

/**
 * Transmits the HOL packet on the channel
 */
void CsmaMac::transmitHOLPacket(){
    dbg_enter("transmitPacket");
    MacPacket* macPacket = encapsulateAppMessage((AppMessage*) buffer.front());
    TransmissionRequest* transRequest = encapsulateMacPacket(macPacket);
    send(transRequest, toTransceiverId);
    currentState = STATE_TCONF;
    dbg_leave("transmitPacket");
}

/**
 * Encapsulates the AppMessage into a MacPacket.
 */
MacPacket* CsmaMac::encapsulateAppMessage(AppMessage* message){
    dbg_enter("encapsulateAppMessage");
    MacPacket* macPacket = new MacPacket;
    macPacket->setReceiverAddress(message->getReceiverAddress());
    macPacket->setTransmitterAddress(ownAddress);
    macPacket->setMacPacketType(MacDataPacket);
    macPacket->setByteLength(macOverheadSizeData);
    macPacket->encapsulate(message->dup());
    dbg_leave("encapsulateAppMessage");
    return macPacket;
}

/**
 * Encapsulates the MacPacket into a TransmissionRequest.
 */
TransmissionRequest* CsmaMac::encapsulateMacPacket(MacPacket* macPacket){
    dbg_enter("encapsulateMacPacket");
    TransmissionRequest* transRequest = new TransmissionRequest;
    transRequest->encapsulate(macPacket);
    dbg_leave("encapsulateMacPacket");
    return transRequest;
}

/**
 * Schedules the backoff message to be received after a given backoff time.
 */
void CsmaMac::beginBackoff(double backOffTime){
    dbg_enter("beginBackoff");
    currentBackoffs++;
    scheduleAt(simTime() + backOffTime, backOffComplete);
    currentState = STATE_BACKOFF;
    dbg_leave("beginBackoff");
}

// ===================================================================================
// ===================================================================================
//
// Debug helpers
//
// ===================================================================================
// ===================================================================================

void CsmaMac::dbg_prefix()
{
    EV << "t = " << simTime()
       << " - CsmaMac-" << ownAddress
       << "s = " << stateStrings[currentState]
       << endl;
}

// ------------------------------------------------------------


void CsmaMac::dbg_enter (std::string methname)
{
    dbg_prefix();
    EV << "entering " << methname
       << endl;
}

// ------------------------------------------------------------

void CsmaMac::dbg_leave (std::string methname)
{
    dbg_prefix();
    EV << "leaving " << methname
       << endl;
}

// ------------------------------------------------------------

void CsmaMac::dbg_string(std::string str)
{
    dbg_prefix();    //delete ackCompletedMessage;
    EV << str
       << endl;
}


/**
 * Deletes any allocated messages.
 */
CsmaMac::~CsmaMac(){
    cancelAndDelete(backOffComplete);
    cancelAndDelete(ackTimeoutMessage);
    cancelAndDelete(ackSendMessage);
    while (buffer.size() > 0) {
        delete buffer.front();
        buffer.pop();
    }
    while (ackQueue.size() > 0) {
        delete ackQueue.front();
        ackQueue.pop();
    }
}

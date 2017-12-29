/** Implements a packet source for magazines
 */

#include "packetmag.h"

using namespace vbit;

// @todo Initialise the magazine data
PacketMag::PacketMag(uint8_t mag, std::list<TTXPageStream>* pageSet, ttx::Configure *configure, uint8_t priority) :
    _pageSet(pageSet),
    _configure(configure),
    _page(nullptr),
    _magNumber(mag),
    _priority(priority),
    _priorityCount(priority),
    _state(PACKETSTATE_HEADER),
    _thisRow(0),
    _lastTxt(nullptr),
    _nextPacket29DC(0)
{
  //ctor
  for (int i=0;i<MAXPACKET29TYPES;i++)
  {
    _packet29[i]=nullptr;
  }
  if (_pageSet->size()>0)
  {
    _it=_pageSet->begin();
    //_it->DebugDump();
    _page=&*_it;
  }
  _carousel=new vbit::Carousel();
  _specialPages=new vbit::SpecialPages();
}

PacketMag::~PacketMag()
{
  //dtor
  delete _carousel;
  delete _specialPages;
}

// @todo Invent a packet sequencer similar to mag.cpp which this will replace
Packet* PacketMag::GetPacket(Packet* p)
{
  // std::cerr << "[PacketMag::GetPacket] mag=" << _magNumber << " state=" << _state << std::endl;
  int thisPageNum;
  unsigned int thisSubcode;
  int thisStatus;
  int* links=NULL;
  TTXLine* tempLine;
  int Packet29Index;
  
  static vbit::Packet* filler=new Packet(8,25,"                                        "); // filler

  // We should only call GetPacket if IsReady has returned true

  /* Nice to have a safety net
   * but without the previous value of the force flag this can give a false positive.

  if (!IsReady())
  {
      std::cerr << "[PacketMag::GetPacket] Packet not ready. This must not happen" << std::endl;
      exit(0);
  }
  */

  // If there is no page, we should send a filler
  if (_pageSet->size()<1)
  {
    p->Set_packet(filler->Get_packet());
    return p;
  }

  switch (_state)
  {
    case PACKETSTATE_HEADER: // Start to send out a new page, which may be a simple page or one of a carousel
        if (GetEvent(EVENT_PACKET_29))
        {
            while (_packet29[_nextPacket29DC] == nullptr)
            {
                _nextPacket29DC++;
            }
            
            if (_nextPacket29DC >= MAXPACKET29TYPES)
            {
                _nextPacket29DC = 0;
                ClearEvent(EVENT_PACKET_29);
            }
            else
            {
                p->SetRow(_magNumber, 29, _packet29[_nextPacket29DC]->GetLine(), CODING_13_TRIPLETS);
                _nextPacket29DC++;
                return p;
            }
        }
        if (GetEvent(EVENT_SPECIAL_PAGES))
        {
            _page=_specialPages->NextPage();
            
            if (_page && _page->GetStatusFlag()==TTXPageStream::MARKED)
            {
                _specialPages->deletePage(_page);
                return nullptr;
            }
            
            if (_page)
            {
                // got a special page
                
                if (_page->GetPageFunction() == MIP)
                {
                    // Magazine Inventory Page
                    ClearEvent(EVENT_FIELD); // enforce 20ms page erasure interval
                }
                
                /* rules for the control bits are complicated. There are rules to allow the page to be sent as fragments. Since we aren't doing that, all the flags are left clear except for C9 (interrupted sequence) to keep special pages out of rolling headers */
                thisStatus = _page->GetPageStatus() & 0x8000; // get transmit flag
                thisStatus |= 0x0010;
                
                /* rules for the subcode are really complicated. The S1 nibble should be the sub page number, S2 is a counter that increments when the page is updated, S3 and S4 hold the last row number that will be transmitted for this page which needs calculating somehow. */
                if (_page->IsCarousel())
                {
                    thisSubcode=(_page->GetCarouselPage()->GetSubCode()) & 0x000F; // will break if carousel has more than 16 subpages but that would be out of spec anyway.
                }
                else
                {
                    thisSubcode = 0; // no subpages
                }
                thisSubcode|=0x2900; // Set the last X/26 row as the final packet for now but this is WRONG
            } else {
                // got to the end of the special pages
                ClearEvent(EVENT_SPECIAL_PAGES);
                return nullptr;
            }
        } else {
            ClearEvent(EVENT_FIELD); // This will suspend all packets until the next field.
            _page=_carousel->nextCarousel(); // The next carousel page (if there is one)

            // But before that, do some housekeeping

            // Is this page deleted?
            if (_page && _page->GetStatusFlag()==TTXPageStream::MARKED)
            {
                _carousel->deletePage(_page);
                _page=nullptr;
            }

            if (_page) // Carousel? Step to the next subpage
            {
                //_outp("c");
                _page->StepNextSubpage();
                //std::cerr << "[PacketMag::GetPacket] Header thisSubcode=" << std::hex << _page->GetCarouselPage()->GetSubCode() << std::endl;
            }
            else  // No carousel? Take the next page in the main sequence
            {
                if (_it==_pageSet->end())
                {
                    std::cerr << "This can not happen (we can't get the next page?)" << std::endl;
                    exit(0);
                }
                ++_it;
                if (_it==_pageSet->end())
                {
                    _it=_pageSet->begin();
                }
                // Get pointer to the page we are sending
                // todo: Find a way to skip carousels without going into an infinite loop
                _page=&*_it;
                // If it is marked for deletion, then remove it.
                if (_page->GetStatusFlag()==TTXPageStream::MARKED)
                {
                    _pageSet->remove(*(_it++));
                    _page=nullptr;
                    return nullptr;
                  // Stays in HEADER mode so that we run this again
                }
                if (_page->IsCarousel() && _page->GetCarouselFlag()) // Don't let registered carousel pages into the main page sequence
                {
                    //std::cerr << "This can not happen. Carousel found but it isn't a carousel?" << std::endl;
                    // exit(0); // @todo MUST FIX THIS. Need to find out how we are getting here and stop it doing that!
                    // Page is a carousel. This can not happen
                    _page=nullptr; // clear everything for now so that we keep running @todo THIS IS AN ERROR
                    return nullptr;
                }
            }
            _thisRow=0;

            // When a single page is changed into a carousel
            if (_page->IsCarousel() != _page->GetCarouselFlag())
            {
              _page->SetCarouselFlag(_page->IsCarousel());
              if (_page->IsCarousel())
              {
                  // std::cerr << "This page has become a carousel. Add it to the list" << std::endl;
                  _carousel->addPage(_page);
              }
              else
              {
                // @todo Implement this
                //std::cerr << "@todo This page has no longer a carousel. Remove it from the list" << std::endl;
                //exit(3); //
              }
            }
            
            if (_page->Special() && _page->GetSpecialFlag()){
                // don't let special pages into normal sequence
                return nullptr;
            }
            
            if (_page->IsCarousel())
            {
                thisSubcode=_page->GetCarouselPage()->GetSubCode();
            }
            else
            {
                thisSubcode=_page->GetSubCode();
            }
            
            thisStatus=_page->GetPageStatus();
            
            // If the page has changed, then set the update bit.
            // This is by request of Nate. It isn't a feature required in ETSI
            if (_page->Changed())
            {
              thisStatus|=PAGESTATUS_C8_UPDATE;
            }
        }
        
        // the page has stopped being special, or become special without getting its flag set
        if (_page->Special() != _page->GetSpecialFlag()){
            _page->SetSpecialFlag(_page->Special());
            if (_page->Special()){
                std::cerr << "[PacketMag::GetPacket] page became special " << std::hex << _page->GetPageNumber() << std::endl;
                _specialPages->addPage(_page);
                return nullptr;
            } else {
                std::cerr << "[PacketMag::GetPacket] page became normal " << std::hex << _page->GetPageNumber() << std::endl;
                _specialPages->deletePage(_page);
            }
        }
        
        // Assemble the header. (we can simplify this code or leave it for the optimiser)
        thisPageNum=_page->GetPageNumber();
        thisPageNum=(thisPageNum/0x100) % 0x100; // Remove this line for Continuous Random Acquisition of Pages.
        
        if ((thisPageNum & 0xFF) == 0xFF)
        {
            // only read packet 29 from page mFF
            //std::cerr << "updating packet 29" << std::endl;
            // TODO: we needn't do this every time round the carousel
            tempLine = _page->GetTxRow(29);
            while (tempLine != nullptr)
            {
                // TODO: when the page is deleted the packets will remain.
                //std::cerr << "page includes packet 29" << std::endl;
                switch (tempLine->GetCharAt(0))
                {
                    case '@':
                        Packet29Index = 0;
                        break;
                    case 'A':
                        Packet29Index = 1;
                        break;
                    case 'D':
                        Packet29Index = 2;
                        break;
                    default:
                        Packet29Index = -1;
                }
                if (Packet29Index > -1)
                {
                    if (_packet29[Packet29Index]==nullptr)
                        _packet29[Packet29Index]=new TTXLine(tempLine->GetLine(),true); // Didn't exist before
                    else
                        _packet29[Packet29Index]->Setm_textline(tempLine->GetLine(), true);
                }
                
                tempLine = tempLine->GetNextLine();
                // loop until every row 29 is copied
            }
        }
        
        if (!(thisStatus & 0x8000))
        {
            _page=nullptr;
            return nullptr;
        }

        // p=new Packet();
        p->Header(_magNumber,thisPageNum,thisSubcode,thisStatus);// loads of stuff to do here!

        p->HeaderText(_configure->GetHeaderTemplate()); // Placeholder 32 characters. This gets replaced later


        //p->Parity(13); // don't apply parity here it will screw up the template. parity for the header is done by tx() later
        assert(p!=NULL);

        links=_page->GetLinkSet();
        if ((links[0] & links[1] & links[2] & links[3] & links[4] & links[5]) != 0x8FF){ // only create if links were initialised
            _state=PACKETSTATE_FASTEXT; // a non zero FL row will override an OL,27 row
            break;
        } else {
            _lastTxt=_page->GetTxRow(27); // Get _lastTxt ready for packet 27 processing
            _state=PACKETSTATE_PACKET27;
            break;
        }
        case PACKETSTATE_PACKET27:
                  //std::cerr << "TRACE-27 " << std::endl;

            if (_lastTxt)
            {
                //std::cerr << "Packet 27 length=" << _lastTxt->GetLine().length() << std::endl;
                //_lastTxt->Dump();
                if ((_lastTxt->GetLine()[0] & 0xF) > 3) // designation codes > 3
                    p->SetRow(_magNumber, 27, _lastTxt->GetLine(), CODING_13_TRIPLETS); // enhancement linking
                else
                    p->SetRow(_magNumber, 27, _lastTxt->GetLine(), CODING_HAMMING_8_4); // navigation packets (TODO: CRC in DC=0 is wrong)
                _lastTxt=_lastTxt->GetNextLine();
                break;
            }
            _lastTxt=_page->GetTxRow(28); // Get _lastTxt ready for packet 28 processing
            _state=PACKETSTATE_PACKET28; //  // Intentional fall through to PACKETSTATE_PACKET28
        case PACKETSTATE_PACKET28:
                  //std::cerr << "TRACE-28 " << std::endl;

            if (_lastTxt)
            {
                //std::cerr << "Packet 28 length=" << _lastTxt->GetLine().length() << std::endl;
                //_lastTxt->Dump();

                p->SetRow(_magNumber, 28, _lastTxt->GetLine(), CODING_13_TRIPLETS);
                _lastTxt=_lastTxt->GetNextLine();
                break;
            }
            else if (_page->GetRegion())
            {
                // create X/28/0 packet for pages which have a region set with RE in file
                // it is important that pages with X/28/0,2,3,4 packets don't set a region otherwise an extra X/28/0 will be generated. TTXPage::SetRow sets the region to 0 for these packets just in case.

                // this could almost certainly be done more efficiently but it's quite confusing and this is more readable for when it all goes wrong.
                std::string val = "@@@tGpCuW@twwCpRA`UBWwDsWwuwwwUwWwuWwE@@"; // default X/28/0 packet
                int region = _page->GetRegion();
                int NOS = (_page->GetPageStatus() & 0x380) >> 7;
                int language = NOS | (region << 3);
                int triplet = 0x3C000 | (language << 7); // construct triplet 1
                val.replace(1,1,1,(triplet & 0x3F) | 0x40);
                val.replace(2,1,1,((triplet & 0xFC0) >> 6) | 0x40);
                val.replace(3,1,1,((triplet & 0x3F000) >> 12) | 0x40);
                //std::cerr << "[PacketMag::GetPacket] region:" << std::hex << region << " nos:" << std::hex << NOS << " triplet:" << std::hex << triplet << std::endl;
                p->SetRow(_magNumber, 28, val, CODING_13_TRIPLETS);
                _lastTxt=_page->GetTxRow(26); // Get _lastTxt ready for packet 26 processing
                _state=PACKETSTATE_PACKET26;
                break;
            } else if (_page->GetPageCoding() == CODING_7BIT_TEXT){
                // X/26 packets next in normal pages
                _lastTxt=_page->GetTxRow(26); // Get _lastTxt ready for packet 26 processing
                _state=PACKETSTATE_PACKET26; // Intentional fall through to PACKETSTATE_PACKET26
            } else {
                // do X/1 to X/25 first and go back to X/26 after
                _state=PACKETSTATE_TEXTROW;
                return nullptr;
            }
        case PACKETSTATE_PACKET26:
            if (_lastTxt)
            {
                p->SetRow(_magNumber, 26, _lastTxt->GetLine(), CODING_13_TRIPLETS);
                // Do we have another line?
                _lastTxt=_lastTxt->GetNextLine();
                // std::cerr << "*";
                break;
            }
            if (_page->GetPageCoding() == CODING_7BIT_TEXT){
                _state=PACKETSTATE_TEXTROW; // Intentional fall through to PACKETSTATE_TEXTROW
            } else {
                // otherwise we end the page here
                _state=PACKETSTATE_HEADER;
                _thisRow=0;
                return nullptr;
            }
    case PACKETSTATE_TEXTROW:
          // std::cerr << "TRACE-T " << std::endl;

      // Find the next row that isn't NULL
      for (_thisRow++;_thisRow<26;_thisRow++)
      {
        // std::cerr << "*";
        _lastTxt=_page->GetTxRow(_thisRow);
        if (_lastTxt!=NULL)
                  break;
      }
      //std::cerr << std::endl;
      //std::cerr << "[PacketMag::GetPacket] TEXT ROW sending MRAG " << (int)_magNumber << " " << (int)_thisRow << std::endl;

      // Didn't find? End of this page.
      if (_thisRow>25 || _lastTxt==NULL)
      {
         // std::cerr << "[PacketMag::GetPacket] FOO row " << std::dec << p->GetRow() << std::endl;
         // std::cerr << p->tx() << std::endl;
        if(_page->GetPageCoding() == CODING_7BIT_TEXT){
          // if this is a normal page we've finished
          _state=PACKETSTATE_HEADER;
          _thisRow=0;
          //_outp("H");
        } else {
          // otherwise go on to X/26
          _lastTxt=_page->GetTxRow(26);
          _state=PACKETSTATE_PACKET26;
        }
        return nullptr;
      }
      else
      {
        //_outp("J");
        if (_lastTxt->IsBlank() && (_configure->GetRowAdaptive() || _page->GetPageFunction() != LOP)) // If a row is empty then skip it if row adaptive mode on, or not a level 1 page
        {
          // std::cerr << "[PacketMag::GetPacket] Empty row" << std::hex << _page->GetPageNumber() << std::dec << std::endl;
          return nullptr;
        }
        else
        {
          // Assemble the packet
          p->SetRow(_magNumber, _thisRow, _lastTxt->GetLine(), _page->GetPageCoding());
          if (_page->GetPageCoding() == CODING_7BIT_TEXT)
              p->Parity(); // only set parity for normal text rows
          assert(p->IsHeader()!=true);
        }
      }
      break;
    case PACKETSTATE_FASTEXT:
                  //std::cerr << "TRACE-F " << std::endl;

//      std::cerr << "PACKETSTATE_FASTEXT enters" << std::endl;
      p->SetMRAG(_magNumber,27);
      links=_page->GetLinkSet();
      p->Fastext(links,_magNumber);
      _lastTxt=_page->GetTxRow(28); // Get _lastTxt ready for packet 28 processing
      _state=PACKETSTATE_PACKET28;
//      std::cerr << "PACKETSTATE_FASTEXT exits" << std::endl;
      break;



  default:
     std::cerr << "TRACE-OOPS " << std::endl;
    _state=PACKETSTATE_HEADER;// For now, do the next page
    return nullptr;
  }

  return p; //
}

/** Is there a packet ready to go?
 *  If the ready flag is set
 *  and the priority count allows a packet to go out
 *  @param force - If true AND if the next packet is being held back due to priority, send the packet anyway
 */
bool PacketMag::IsReady(bool force)
{
  bool result=false;
  // We can always send something unless
  // 1) We have just sent out a header and are waiting on a new field
  // 2) There are no pages
  if ( ((GetEvent(EVENT_FIELD)) || (_state==PACKETSTATE_HEADER)) && (_pageSet->size()>0))
  {
    // If we send a header we want to wait for this to get set GetEvent(EVENT_FIELD)
    _priorityCount--;
    if (_priorityCount==0 || force)
    {
      _priorityCount=_priority;
      result=true;
    }
  }
  /*
  std::cerr << "[PacketMag::IsReady] exits."
    " mag=" << _magNumber <<
    " force=" << force <<
    " result=" << result <<
    " EVENT_FIELD=" << GetEvent(EVENT_FIELD) <<
    std::endl;
    */
  return result;
};
